#pragma once

/*
 * ============================================================================
 *
 *        CuckooTable
 *          A compact hash table storing fixed-length fingerprints in a
 *          cuckoo filter. The monitored-key set of Sublime_MG and of the
 *          plain Misra-Gries baseline beside it.
 *
 * ============================================================================
 *
 * This replaced a rank-and-select quotient filter, and the reason is shifting.
 * An RSQF keeps its runs in slot order, so an insertion or a deletion slides a
 * whole cluster along -- and at the load factor Misra-Gries runs its table at,
 * a cluster is hundreds of slots, each of which drags its counter, and the min
 * tree's repair of that counter, with it. A cuckoo filter never shifts
 * anything. An entry lives in one of two buckets, an insertion that finds both
 * full relocates exactly *one* entry per kick, and a deletion clears a slot
 * and stops. The counter array therefore only ever sees point updates, which
 * is what makes the min segment tree over it affordable.
 *
 * `SublimeMG` and `MG` still take their table as a template parameter, so
 * another one can be dropped in, but this is the only one in the tree.
 *
 * ---------------------------------------------------------------------------
 * Hashing
 * ---------------------------------------------------------------------------
 * A key is hashed to `key_bits` bits and split:
 *
 *      bucket_index_hash_size (BIHS) = log2(bucket_count)
 *      primary bucket = hash bits [0, BIHS)
 *      fingerprint    = hash bits [BIHS, BIHS + L)
 *
 * The alternate bucket is the usual partial-key one, `b ^ mix(fingerprint)`,
 * so a lookup reads two buckets and a kick can re-place an entry knowing only
 * what is stored. A fingerprint of zero marks an empty slot, so a hash that
 * would produce one uses 1 instead; the bias is one value in `2^L`.
 *
 * ---------------------------------------------------------------------------
 * The flag bit, and why expansion needs it
 * ---------------------------------------------------------------------------
 * Partial-key hashing is symmetric: from a stored entry you can tell which
 * *pair* of buckets it belongs to, but not which of the two is the primary --
 * and the primary is exactly the hash bits Sublime needs in order to split a
 * bucket when the table grows. So every slot carries one more bit saying
 * whether its entry sits in its primary bucket or its alternate. With it the
 * whole `key_bits` hash is recoverable:
 *
 *      primary = flag ? bucket ^ mix(fingerprint) : bucket
 *      hash    = primary | (fingerprint << BIHS)
 *
 * and an expansion is a plain re-hash of every entry. The bit is cheap: a
 * cuckoo filter has none of the quotient filter's per-block occupied, runend
 * and offset metadata, which cost 2.1 bits per slot.
 *
 * ---------------------------------------------------------------------------
 * Growing: deeper buckets, then more of them
 * ---------------------------------------------------------------------------
 * The bucket count has to stay a power of two, since the alternate bucket is
 * an XOR, so Zeno filter's Stretching has nothing to stretch. Depth takes its
 * place. An expansion gives every bucket `base_depth / r` more slots; when
 * that would take the depth to twice its base, the table instead doubles its
 * buckets, drops back to the base depth, and sheds a fingerprint bit -- the
 * period ending, exactly as before. So the growth coefficient `r` is how many
 * steps a period takes, it has to divide the base depth, and `r = 1` is plain
 * doubling.
 *
 * Unlike Stretching, the steps are not all the same size: at a base depth of 4
 * and `r = 4` they are 5/4, 6/5, 7/6 and 8/7. A period still doubles.
 *
 * ---------------------------------------------------------------------------
 * Prefetching: both buckets at once
 * ---------------------------------------------------------------------------
 * A lookup's two buckets are known before either is read, and they are two
 * independent cache misses that the obvious code puts in series -- read the
 * primary, find nothing, read the alternate. Issuing a prefetch for both
 * before reading either lets the two travel together, which is worth 5-7% of
 * insert and query time once the table is larger than the last-level cache
 * (measured at 1M slots; at 64k, where it all fits, the difference is inside
 * the noise either way).
 *
 * Only the *slot words* are prefetched. Asking for the counters of both
 * buckets as well was tried and is slightly worse than asking for neither:
 * at most one of the two buckets holds the entry, so one of the two counter
 * prefetches is always wasted. `FindMatch` does still prefetch the primary
 * bucket's counter chunk, whose address the bucket alone fixes.
 *
 * ---------------------------------------------------------------------------
 * What this does not guarantee: keeping what it took
 * ---------------------------------------------------------------------------
 * A kick path can run out of patience with room still left in the table, and
 * then the entry it is carrying has nowhere to go. If that is the arrival,
 * `InsertAt` reports `err_no_space` and nothing was stored; if it is an older
 * tenant -- the arrival having been placed and picked up again along the way --
 * the arrival stays and the tenant is dropped, count and all, which
 * `CountLostEntries` reports. Both are rare (a few hundred over a 300k-update
 * Misra-Gries stream at a 0.95 load factor) and both are silent, so a caller
 * that cares about accuracy should read that counter rather than assume the
 * table still holds everything it accepted.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "TableHashing.hpp"          // `MurmurHash64A`, `hash_64` and `fpt::`.
#include "VALECounters.hpp"
#include "util.hpp"

namespace sublime {

class CuckooTable {
    friend class CuckooTableTest;

public:
    /**
     * How a key becomes the hash the table splits into a bucket and a
     * fingerprint: `Default` hashes it with Murmur, `Invertible` with a
     * reversible integer hash as wide as the key, and `None` uses the key as
     * its own hash -- which makes skew in the input skew in the load.
     */
    enum class hashmode {
        Default,
        Invertible,
        None
    };

    /** Signals that the key passed in has already been hashed. */
    static constexpr uint32_t flag_key_is_hash = 0x08;

    /* Status codes. */
    static constexpr int32_t err_no_space = -1;
    static constexpr int32_t err_doesnt_exist = -3;
    static constexpr int32_t err_cannot_contract = -4;

    /** The occupancy an insertion is expected to reach before it starts failing. */
    static constexpr double max_load_factor = 0.95;

    /** Slots per bucket before any expansion. Four is the usual cuckoo bucket. */
    static constexpr uint32_t default_base_depth = 4;

    /** How many entries one insertion may relocate before it gives up. */
    static constexpr uint32_t max_kicks = 500;

    /**
     * @param nslots The number of slots wanted. The bucket count has to be a
     * power of two, so it is the largest one that fits and **the slack goes
     * into the depth**: the table comes out with at most `nslots` slots and
     * within one bucket of it. Rounding the bucket count *up* instead would
     * make a table up to twice the size asked for, which a caller sizing the
     * summary to a byte budget can only answer by asking for half of it.
     * @param key_bits The number of bits of the hash the table uses. The
     * fingerprint gets whatever is left over the bucket index.
     * @param hash_mode See `hashmode`.
     * @param seed The seed of the hash function.
     * @param growth_coefficient `r`: how many expansions a period takes, one
     * being plain doubling. A period adds `base_depth` slots to every bucket in
     * `r` equal steps, so `r` has to divide the base depth -- which is why the
     * base depth is *derived* from `r` unless one is asked for: the smallest
     * multiple of `r` that is at least `default_base_depth`. So `r` of 1, 2 or
     * 4 gives buckets of 4, `r` of 3 gives 6, and any `r` is usable.
     * @param base_depth Slots per bucket at the start of a period, or 0 to
     * derive it from `r`.
     */
    /** The bucket count, depth and base depth a `nslots` request comes out as. */
    struct Shape {
        uint64_t buckets;
        uint32_t depth;
        uint32_t base_depth;
    };

    /**
     * Works out that shape without building anything. The bucket count has to
     * be a power of two, so it is the largest one that fits and the slack goes
     * into the depth; the base depth is derived from `r` unless given.
     */
    static Shape ShapeFor(uint64_t nslots, uint32_t growth_coefficient = 1,
                          uint32_t base_depth = 0) {
        assert(growth_coefficient >= 1);
        if (base_depth == 0)
            base_depth = growth_coefficient
                            * ((default_base_depth + growth_coefficient - 1) / growth_coefficient);
        assert(base_depth % growth_coefficient == 0);
        uint64_t buckets = 1;
        while (buckets * 2 * base_depth <= nslots)
            buckets <<= 1;
        // A depth over the base is a table starting part-way into a period:
        // every invariant here is `depth in [base_depth, 2 * base_depth)`, and
        // only the growth ratio of the *first* expansion is irregular.
        const uint32_t depth = buckets * base_depth <= nslots
                                    ? static_cast<uint32_t>(nslots / buckets)
                                    : base_depth;
        return {buckets, depth, base_depth};
    }

    /**
     * @returns The `key_bits` that make a freshly stored fingerprint
     * `fingerprint_bits` long in a table of this shape.
     *
     * Not `log2(nslots) + fingerprint_bits`: only the *bucket* index is taken
     * from the hash, and a bucket holds `base_depth` slots, so a 1024-slot
     * table with the default depth of four hashes 8 bits, not 10. Getting this
     * wrong does not fail, it silently stores fingerprints two bits longer than
     * asked for, and pays for them in every slot.
     */
    static uint64_t KeyBitsFor(uint64_t nslots, uint32_t fingerprint_bits,
                               uint32_t growth_coefficient = 1, uint32_t base_depth = 0) {
        const Shape shape = ShapeFor(nslots, growth_coefficient, base_depth);
        uint64_t bucket_bits = 0;
        while ((1ULL << bucket_bits) < shape.buckets)
            bucket_bits++;
        return bucket_bits + fingerprint_bits;
    }

    explicit CuckooTable(uint64_t nslots, uint64_t key_bits, hashmode hash_mode, uint32_t seed,
                         uint32_t growth_coefficient = 1, uint32_t base_depth = 0):
            hash_mode_{hash_mode}, seed_{seed} {
        const Shape shape = ShapeFor(nslots, growth_coefficient, base_depth);
        base_depth_ = shape.base_depth;
        growth_coefficient_ = growth_coefficient;
        allocate(shape.buckets, shape.depth, key_bits);
        original_bucket_count_ = shape.buckets;
        rng_state_ = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    }

    CuckooTable(const CuckooTable& other) {
        *this = other;
    }
    CuckooTable& operator=(const CuckooTable& other) {
        if (this == &other)
            return *this;
        copy_shape_from(other);
        words_ = other.words_;
        entry_count_ = other.entry_count_;
        counters_ = other.counters_ ? std::make_unique<VALECounters>(*other.counters_) : nullptr;
        sidecar_ = nullptr;             // A sidecar belongs to its owner, not to a copy.
        return *this;
    }
    CuckooTable(CuckooTable&& other) noexcept {
        *this = std::move(other);
    }
    CuckooTable& operator=(CuckooTable&& other) noexcept {
        copy_shape_from(other);
        words_ = std::move(other.words_);
        entry_count_ = other.entry_count_;
        counters_ = std::move(other.counters_);
        sidecar_ = other.sidecar_;
        other.entry_count_ = 0;
        return *this;
    }

    /* Lookup. */

    /**
     * @returns The slot holding the stored fingerprint matching `key`, or -1
     * if neither of its buckets holds one. The counter chunk of the primary
     * bucket goes out before any slot is read, so that the two misses travel
     * together rather than queueing up.
     */
    int64_t FindMatch(uint64_t key, uint8_t flags = 0) const {
        const uint64_t hash = hash_key(key, flags);
        const uint64_t fingerprint = fingerprint_from_hash(hash);
        const uint64_t primary = bucket_from_hash(hash);
        const uint64_t other = alternate(primary, fingerprint);
        prefetch_bucket(primary);
        if (other != primary)
            prefetch_bucket(other);
        if (counters_ != nullptr)
            counters_->Prefetch(primary * depth_);

        const int64_t at = find_in_bucket(primary, fingerprint);
        return at >= 0 ? at : find_in_bucket(other, fingerprint);
    }

    bool Contains(uint64_t key, uint8_t flags = 0) const {
        return FindMatch(key, flags) >= 0;
    }

    /** @returns How many stored fingerprints match `key`'s. */
    uint64_t Count(uint64_t key, uint8_t flags = 0) const {
        const uint64_t hash = hash_key(key, flags);
        const uint64_t fingerprint = fingerprint_from_hash(hash);
        const uint64_t primary = bucket_from_hash(hash);
        uint64_t res = count_in_bucket(primary, fingerprint);
        const uint64_t other = alternate(primary, fingerprint);
        if (other != primary)
            res += count_in_bucket(other, fingerprint);
        return res;
    }

    /* Update. */

    /**
     * Stores a fingerprint for `key`, relocating other entries if both of its
     * buckets are full.
     *
     * @returns The slot it landed in -- which the kick path keeps track of,
     * since a later kick can pick up the very entry this insertion placed --
     * or `err_no_space` if the kicking ran out of patience, which a cuckoo
     * filter can do with room still left in the table.
     */
    int64_t InsertAt(uint64_t key, uint8_t flags = 0) {
        const uint64_t hash = hash_key(key, flags);
        return insert_hash_at(hash);
    }

    /** As `InsertAt`, without reporting the slot. */
    int32_t Insert(uint64_t key, uint8_t flags = 0) {
        const int64_t at = InsertAt(key, flags);
        return at < 0 ? static_cast<int32_t>(at) : 0;
    }

    /**
     * Removes one stored fingerprint matching `key`.
     *
     * @returns 0, or `err_doesnt_exist`.
     */
    int32_t Delete(uint64_t key, uint8_t flags = 0) {
        const int64_t at = FindMatch(key, flags);
        if (at < 0)
            return err_doesnt_exist;
        DeleteSlot(BucketOfSlot(at), static_cast<uint64_t>(at));
        return 0;
    }

    /** Removes the entry in `slot`; `bucket` is accepted for interface parity. */
    void DeleteSlot(uint64_t bucket, uint64_t slot) {
        (void) bucket;
        assert(read_slot(slot) != 0);
        write_slot(slot, 0);
        entry_count_--;
        if (counters_ != nullptr)
            counters_->Set(slot, 0);
        if (sidecar_ != nullptr)
            sidecar_->Clear(slot);
    }

    /** Empties the table without changing its shape. */
    void Reset() {
        std::fill(words_.begin(), words_.end(), 0);
        entry_count_ = 0;
        if (counters_ != nullptr)
            counters_->Reset();
        if (sidecar_ != nullptr)
            sidecar_->Reset();
    }

    /* Shape. */

    uint64_t CountSlots() const {
        return bucket_count_ * depth_;
    }
    /** No run ever spills past its bucket, so this is the slot count. */
    uint64_t GetSlotCapacity() const {
        return CountSlots();
    }
    uint64_t CountFingerprints() const {
        return entry_count_;
    }
    uint64_t CountOccupiedSlots() const {
        return entry_count_;
    }
    double LoadFactor() const {
        return static_cast<double>(entry_count_) / CountSlots();
    }
    uint64_t GetNumKeyBits() const {
        return key_bits_;
    }
    uint64_t GetNumFingerprintBits() const {
        return fingerprint_bits_;
    }
    uint64_t GetBitsPerSlot() const {
        return bits_per_slot_;
    }
    uint64_t GetBucketIndexHashSize() const {
        return bucket_bits_;
    }
    uint64_t GetBucketCount() const {
        return bucket_count_;
    }
    uint64_t GetBucketDepth() const {
        return depth_;
    }
    uint32_t GetGrowthCoefficient() const {
        return growth_coefficient_;
    }
    uint32_t GetEpoch() const {
        return (depth_ - base_depth_) / depth_step();
    }
    uint64_t GetPeriodCount() const {
        uint64_t periods = 0;
        for (uint64_t b = bucket_count_; b > original_bucket_count_; b >>= 1)
            periods++;
        return periods;
    }
    uint64_t GetExpansionCount() const {
        return GetPeriodCount() * growth_coefficient_ + GetEpoch();
    }
    uint64_t SizeInBytes() const {
        return words_.capacity() * sizeof(uint64_t);
    }
    hashmode GetHashMode() const {
        return hash_mode_;
    }
    uint32_t GetHashSeed() const {
        return seed_;
    }
    /** @returns How many entries a kick path has given up on and lost. */
    uint64_t CountLostEntries() const {
        return lost_entries_;
    }

    /** @returns The bucket the entry in `slot` belongs to. */
    uint64_t BucketOfSlot(uint64_t slot) const {
        return slot / depth_;
    }

    /**
     * @returns The bits of `key`'s hash the table keeps. Two keys share this
     * exactly when no insertion or query can tell them apart.
     */
    uint64_t EntryIdentity(uint64_t key, uint8_t flags = 0) const {
        const uint64_t hash = hash_key(key, flags);
        const uint64_t fingerprint = fingerprint_from_hash(hash);
        const uint64_t primary = bucket_from_hash(hash);
        return std::min(primary, alternate(primary, fingerprint))
                    | (fingerprint << bucket_bits_);
    }

    /* Growing and shrinking. */

    /**
     * @returns The slot count one expansion would leave behind, or the current
     * count if the fingerprint has no bit left to spend on one.
     */
    uint64_t CountSlotsAfterExpansion() const {
        const uint32_t grown = depth_ + depth_step();
        if (grown >= 2 * base_depth_ && fingerprint_bits_ <= 1)
            return CountSlots();
        return bucket_count_ * grown;
    }
    uint64_t CountSlotsAfterContraction() const {
        if (GetExpansionCount() == 0)
            return CountSlots();
        return bucket_count_ * (depth_ - depth_step());
    }

    /**
     * Deepens every bucket, or -- when that would take the depth to twice its
     * base -- doubles the buckets instead, drops the depth back and takes a
     * bit off every fingerprint.
     *
     * @returns The number of entries afterwards, or a negative status code.
     * The table is left untouched on failure.
     */
    int64_t Expand() {
        const uint32_t grown = depth_ + depth_step();
        const bool ending_period = (grown >= 2 * base_depth_);
        if (ending_period && fingerprint_bits_ <= 1)
            return err_no_space;

        CuckooTable rebuilt(*this, ending_period ? bucket_count_ * 2 : bucket_count_,
                            ending_period ? base_depth_ : grown);
        const int64_t res = rebuild_into(rebuilt);
        if (res < 0)
            return res;
        *this = std::move(rebuilt);
        return res;
    }

    /** The exact inverse of `Expand`. */
    int64_t Contract() {
        if (GetExpansionCount() == 0)
            return err_cannot_contract;
        const bool reopening_period = (depth_ == base_depth_);
        CuckooTable rebuilt(*this, reopening_period ? bucket_count_ / 2 : bucket_count_,
                            reopening_period ? 2 * base_depth_ - depth_step() : depth_ - depth_step());
        const int64_t res = rebuild_into(rebuilt);
        if (res < 0)
            return res;
        *this = std::move(rebuilt);
        return res;
    }

    /* Counters and sidecars. */

    void EnableCounters(bool with_min_tree = false) {
        counters_ = std::make_unique<VALECounters>(GetSlotCapacity(), with_min_tree);
    }
    void DisableCounters() {
        counters_.reset();
    }
    bool CountersEnabled() const {
        return counters_ != nullptr;
    }
    VALECounters *GetCounters() {
        return counters_.get();
    }
    const VALECounters *GetCounters() const {
        return counters_.get();
    }

    /**
     * A caller-owned per-slot array the table keeps aligned to its slots.
     * Nothing here moves a range: an entry is placed, moved one slot at a time
     * by a kick, carried in hand between two slots of a kick path, or cleared.
     */
    struct SlotMirror {
        virtual ~SlotMirror() = default;
        virtual void MoveSlot(uint64_t from, uint64_t to) = 0;
        /** Exchanges `slot`'s data with the sidecar's one-entry hand. */
        virtual void SwapWithHeld(uint64_t slot) = 0;
        /** Empties that hand, before a kick path starts carrying. */
        virtual void ClearHeld() = 0;
        virtual void Clear(uint64_t slot) = 0;
        virtual void Reset() = 0;
    };

    void AttachMirror(SlotMirror *mirror) {
        sidecar_ = mirror;
    }

    /* Iteration, in slot order. */

    class const_iterator {
    public:
        const_iterator(const CuckooTable& table, uint64_t at): table_{&table}, at_{at} {
            skip_empty();
        }
        bool operator==(const const_iterator& rhs) const {
            return at_ == rhs.at_;
        }
        bool operator!=(const const_iterator& rhs) const {
            return at_ != rhs.at_;
        }
        const_iterator& operator++() {
            at_++;
            skip_empty();
            return *this;
        }
        uint64_t slot() const {
            return at_;
        }
        uint64_t bucket() const {
            return at_ / table_->depth_;
        }
        uint64_t fingerprint() const {
            return table_->fingerprint_of(table_->read_slot(at_));
        }
        /** @returns The `key_bits`-wide hash of the entry pointed at. */
        uint64_t hash() const {
            return table_->hash_of(bucket(), table_->read_slot(at_));
        }

    private:
        void skip_empty() {
            while (at_ < table_->CountSlots() && table_->read_slot(at_) == 0)
                at_++;
        }

        const CuckooTable *table_;
        uint64_t at_;
    };

    const_iterator begin() const {
        return const_iterator(*this, 0);
    }
    const_iterator end() const {
        return const_iterator(*this, CountSlots());
    }

    /* Hashing, exposed for the tests to check the splitting against. */

    uint64_t hash_key(uint64_t key, uint8_t flags) const {
        if ((flags & flag_key_is_hash) == 0) {
            if (hash_mode_ == hashmode::Default)
                key = MurmurHash64A(&key, sizeof(key), seed_);
            else if (hash_mode_ == hashmode::Invertible)
                key = hash_64(key, fpt::bitmask(63));
        }
        return key;
    }

    uint64_t bucket_from_hash(uint64_t hash) const {
        return hash & (bucket_count_ - 1);
    }

    uint64_t fingerprint_from_hash(uint64_t hash) const {
        const uint64_t fingerprint = (hash >> bucket_bits_) & fpt::bitmask(fingerprint_bits_);
        return fingerprint == 0 ? 1 : fingerprint;      // Zero marks an empty slot.
    }

    /** @returns The other bucket an entry with this fingerprint could be in. */
    uint64_t alternate(uint64_t bucket, uint64_t fingerprint) const {
        return (bucket ^ (fingerprint * 0x5bd1e995ULL)) & (bucket_count_ - 1);
    }

private:
    /** Builds an empty table of a given shape, keeping everything else. */
    CuckooTable(const CuckooTable& like, uint64_t buckets, uint32_t depth):
            hash_mode_{like.hash_mode_}, seed_{like.seed_} {
        base_depth_ = like.base_depth_;
        growth_coefficient_ = like.growth_coefficient_;
        original_bucket_count_ = like.original_bucket_count_;
        rng_state_ = like.rng_state_;
        allocate(buckets, depth, like.key_bits_);
        if (like.counters_ != nullptr)
            counters_ = std::make_unique<VALECounters>(GetSlotCapacity(), *like.counters_);
    }

    void allocate(uint64_t buckets, uint32_t depth, uint64_t key_bits) {
        assert(buckets >= 1 && (buckets & (buckets - 1)) == 0);
        bucket_count_ = buckets;
        depth_ = depth;
        key_bits_ = key_bits;
        bucket_bits_ = 0;
        for (uint64_t b = buckets; b > 1; b >>= 1)
            bucket_bits_++;
        assert(key_bits_ > bucket_bits_);
        fingerprint_bits_ = key_bits_ - bucket_bits_;
        bits_per_slot_ = fingerprint_bits_ + 1;         // One more for the flag.
        assert(bits_per_slot_ <= 56);
        entry_count_ = 0;
        // One guard word, so a slot straddling the last boundary can be read
        // and written with an unconditional two-word access.
        words_.assign((CountSlots() * bits_per_slot_ + 63) / 64 + 1, 0);
    }

    uint32_t depth_step() const {
        return base_depth_ / growth_coefficient_;
    }

    /* Slot access. */

    uint64_t read_slot(uint64_t slot) const {
        const uint64_t bit = slot * bits_per_slot_;
        const uint64_t word = bit >> 6, offset = bit & 63;
        uint64_t value = words_[word] >> offset;
        if (offset + bits_per_slot_ > 64)
            value |= words_[word + 1] << (64 - offset);
        return value & fpt::bitmask(bits_per_slot_);
    }

    void write_slot(uint64_t slot, uint64_t value) {
        assert(value <= fpt::bitmask(bits_per_slot_));
        const uint64_t bit = slot * bits_per_slot_;
        const uint64_t word = bit >> 6, offset = bit & 63;
        const uint64_t mask = fpt::bitmask(bits_per_slot_);
        words_[word] = (words_[word] & ~(mask << offset)) | (value << offset);
        if (offset + bits_per_slot_ > 64) {
            const uint32_t low = 64 - offset;
            words_[word + 1] = (words_[word + 1] & ~(mask >> low)) | (value >> low);
        }
    }

    /**
     * Brings a bucket's slot words, and the counters beside them, into cache.
     *
     * Both candidate buckets are known before either is read, and they are two
     * independent misses in the same dependency chain: reading the first, then
     * finding the fingerprint absent and reading the second, serialises them.
     * Issuing both prefetches first lets the two go out together.
     */
    __attribute__((always_inline))
    void prefetch_bucket(uint64_t bucket) const {
        const uint64_t first_bit = bucket * depth_ * bits_per_slot_;
        const uint64_t last_bit = first_bit + depth_ * bits_per_slot_ - 1;
        __builtin_prefetch(words_.data() + (first_bit >> 6));
        if ((last_bit >> 6) != (first_bit >> 6))
            __builtin_prefetch(words_.data() + (last_bit >> 6));
    }

    uint64_t fingerprint_of(uint64_t stored) const {
        return stored & fpt::bitmask(fingerprint_bits_);
    }
    bool flag_of(uint64_t stored) const {
        return (stored >> fingerprint_bits_) & 1;
    }
    uint64_t fingerprint_at(uint64_t fingerprint, bool in_alternate) const {
        return fingerprint | (static_cast<uint64_t>(in_alternate) << fingerprint_bits_);
    }

    /** @returns The whole hash of an entry, which is what a resize re-splits. */
    uint64_t hash_of(uint64_t bucket, uint64_t stored) const {
        const uint64_t fingerprint = fingerprint_of(stored);
        const uint64_t primary = flag_of(stored) ? alternate(bucket, fingerprint) : bucket;
        return primary | (fingerprint << bucket_bits_);
    }

    /** @returns A slot of `bucket` holding `fingerprint`, or -1. */
    int64_t find_in_bucket(uint64_t bucket, uint64_t fingerprint) const {
        const uint64_t first = bucket * depth_;
        for (uint64_t j = 0; j < depth_; j++)
            if (fingerprint_of(read_slot(first + j)) == fingerprint)
                return static_cast<int64_t>(first + j);
        return -1;
    }

    uint64_t count_in_bucket(uint64_t bucket, uint64_t fingerprint) const {
        const uint64_t first = bucket * depth_;
        uint64_t res = 0;
        for (uint64_t j = 0; j < depth_; j++)
            res += (fingerprint_of(read_slot(first + j)) == fingerprint);
        return res;
    }

    void copy_shape_from(const CuckooTable& other) {
        hash_mode_ = other.hash_mode_;
        seed_ = other.seed_;
        bucket_count_ = other.bucket_count_;
        original_bucket_count_ = other.original_bucket_count_;
        depth_ = other.depth_;
        base_depth_ = other.base_depth_;
        growth_coefficient_ = other.growth_coefficient_;
        key_bits_ = other.key_bits_;
        bucket_bits_ = other.bucket_bits_;
        fingerprint_bits_ = other.fingerprint_bits_;
        bits_per_slot_ = other.bits_per_slot_;
        rng_state_ = other.rng_state_;
    }

    uint64_t next_random() {
        rng_state_ ^= rng_state_ << 13;
        rng_state_ ^= rng_state_ >> 7;
        rng_state_ ^= rng_state_ << 17;
        return rng_state_;
    }

    /** Exchanges what `slot` holds with what the kick path is carrying. */
    void swap_payload(uint64_t slot, uint64_t& carried_count) {
        if (counters_ != nullptr) {
            // On the *rebased* scale, so that a count carried across the
            // frontier of a merge pass -- see `VALECounters::BeginOffset` --
            // has whatever that pass has already taken off applied or undone
            // as it lands. A kick path knows nothing of any of that; it moves a
            // count from one slot to another and the array keeps the books.
            const uint64_t there = counters_->GetRebased(slot);
            counters_->SetRebased(slot, carried_count, there);
            carried_count = there;
        }
        if (sidecar_ != nullptr)
            sidecar_->SwapWithHeld(slot);
    }

    /**
     * Places a fingerprint derived from `hash`, kicking if it has to.
     *
     * A kick takes the entry sitting in a slot, puts the one in hand there,
     * and goes looking for a home for the one just displaced -- in *its* other
     * bucket, which is why the flag flips. One entry moves per kick, with its
     * count, where the RSQF would have moved a whole cluster.
     *
     * The count travels *in hand* rather than slot to slot: each step swaps
     * what it is carrying with what the slot holds. Moving it slot to slot
     * would overwrite the displaced entry's count before reading it, and the
     * swap is also what makes a kick path that loops back on itself harmless.
     *
     * @returns The slot the caller's entry ended up in -- which is tracked,
     * since a later kick can pick that entry up too. If the kicks run out the
     * entry in hand has nowhere to go and is lost, which is what a cuckoo
     * filter does; the caller's entry is stored either way.
     */
    int64_t insert_hash_at(uint64_t hash) {
        const uint64_t fingerprint = fingerprint_from_hash(hash);
        const uint64_t primary = bucket_from_hash(hash);
        const uint64_t other = alternate(primary, fingerprint);
        prefetch_bucket(primary);
        if (other != primary)
            prefetch_bucket(other);

        int64_t at = find_in_bucket(primary, 0);
        if (at >= 0) {
            write_slot(at, fingerprint_at(fingerprint, false));
            entry_count_++;
            return at;
        }
        at = find_in_bucket(other, 0);
        if (at >= 0) {
            write_slot(at, fingerprint_at(fingerprint, true));
            entry_count_++;
            return at;
        }

        // Both buckets are full, so a tenant of one of them makes way. The
        // arrival goes in with nothing to its name; its caller sets the count.
        if (sidecar_ != nullptr)
            sidecar_->ClearHeld();
        uint64_t bucket = (next_random() & 1) ? primary : other;
        uint64_t carried = fingerprint_at(fingerprint, bucket == other);
        uint64_t carried_count = 0;
        bool arrival_in_hand = true;
        uint64_t landed = 0;
        entry_count_++;

        for (uint32_t kick = 0; kick <= max_kicks; kick++) {
            const int64_t free_slot = find_in_bucket(bucket, 0);
            const uint64_t target = free_slot >= 0
                                        ? static_cast<uint64_t>(free_slot)
                                        : bucket * depth_ + (next_random() % depth_);
            const uint64_t displaced = free_slot >= 0 ? 0 : read_slot(target);
            const bool displaced_arrival = (!arrival_in_hand && target == landed);

            swap_payload(target, carried_count);
            write_slot(target, carried);
            if (arrival_in_hand)
                landed = target;

            if (free_slot >= 0)
                return static_cast<int64_t>(landed);

            // The displaced entry is now in hand, headed for its other bucket.
            // This runs on the last kick too, so that what the loop leaves in
            // hand is what the code below thinks it is.
            carried = fingerprint_at(fingerprint_of(displaced), !flag_of(displaced));
            arrival_in_hand = displaced_arrival;
            bucket = alternate(bucket, fingerprint_of(displaced));
        }
        // Out of patience with an entry still in hand.
        if (arrival_in_hand) {
            // It is the caller's own entry that has nowhere to go, so nothing
            // was added and nothing was lost; the insertion simply failed.
            entry_count_--;
            return err_no_space;
        }
        // Otherwise the caller's entry is stored and an older one is in hand
        // with nowhere to go. The table loses that one, and with it whatever
        // it was counting.
        lost_entries_++;
        entry_count_--;
        return static_cast<int64_t>(landed);
    }

    /** Re-inserts every entry of this table into `dest`, which is shaped anew. */
    int64_t rebuild_into(CuckooTable& dest) const {
        if (dest.counters_ != nullptr)
            dest.counters_->SuspendTree();
        int64_t moved = 0;
        for (auto it = begin(); it != end(); ++it) {
            const uint64_t hash = it.hash();
            const uint64_t count = (counters_ != nullptr ? counters_->Get(it.slot()) : 0);
            const int64_t at = dest.insert_hash_at(hash);
            if (at < 0)
                return at;
            if (dest.counters_ != nullptr)
                dest.counters_->Set(at, count);
            moved++;
        }
        if (dest.counters_ != nullptr)
            dest.counters_->RebuildTree();
        return moved;
    }

    hashmode hash_mode_;
    uint32_t seed_;
    uint64_t bucket_count_ = 0;
    uint64_t original_bucket_count_ = 0;
    uint32_t depth_ = 0;
    uint32_t base_depth_ = 0;
    uint32_t growth_coefficient_ = 1;
    uint64_t key_bits_ = 0;
    uint64_t bucket_bits_ = 0;
    uint64_t fingerprint_bits_ = 0;
    uint64_t bits_per_slot_ = 0;
    uint64_t entry_count_ = 0;
    /** Entries the kick path ran out of patience with and dropped. */
    uint64_t lost_entries_ = 0;
    uint64_t rng_state_ = 1;
    std::vector<uint64_t> words_;
    std::unique_ptr<VALECounters> counters_;
    SlotMirror *sidecar_ = nullptr;
};

}   // namespace sublime
