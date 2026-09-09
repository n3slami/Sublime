#pragma once

/*
 * ============================================================================
 *
 *        FingerprintTable
 *          A compact hash table storing a multiset of variable-length
 *          fingerprints. It is the building block of Sublime_MG, i.e., the
 *          Sublime framework applied to the Misra-Gries summary.
 *
 *        Derived from Memento filter
 *          Author:   Navid Eslami
 *
 *        which is in turn derived from the RSQF
 *          Authors:  Prashant Pandey <ppandey@cs.stonybrook.edu>
 *                    Rob Johnson <robj@vmware.com>
 *
 * ============================================================================
 *
 * This is a rank-and-select quotient filter (RSQF) with Memento filter's
 * hashing semantics, Aleph filter's variable-length fingerprints, and Zeno
 * filter's Stretching, but with all of Memento filter's memento/keepsake-box
 * machinery removed. Every stored item occupies exactly one slot holding just
 * a fingerprint, which makes the run encoding considerably simpler than
 * Memento filter's.
 *
 * ---------------------------------------------------------------------------
 * Hashing
 * ---------------------------------------------------------------------------
 * Identical to Memento filter. A key is hashed, and then the lowest
 * `original_quotient_bits` of the hash are fast-reduced into the range of the
 * table's *original* slot count, so that the table may start at a size that is
 * not a power of two. The resulting value is what we call "the hash" below.
 * From it we derive:
 *
 *      bucket_index_hash_size (BIHS) = key_bits - fingerprint_bits
 *      bucket = [fast-reduced original quotient : hash bits [OQB, BIHS)]
 *      fingerprint = hash bits [BIHS, BIHS + L)
 *
 * Expanding the table by a factor of two increases BIHS by one, so the
 * *least* significant bit of every fingerprint is donated to the bucket index.
 *
 * ---------------------------------------------------------------------------
 * Variable-length fingerprints (Aleph filter)
 * ---------------------------------------------------------------------------
 * A slot is `fingerprint_bits + 1` bits wide. A fingerprint of length `L` is
 * stored as
 *
 *      (1 << L) | (hash bits [BIHS, BIHS + L))
 *
 * i.e., a "void bit" is prepended to the fingerprint, and its position encodes
 * the fingerprint's length. Two fingerprints match when they agree on the
 * lowest `min(L1, L2)` bits, so a shorter stored fingerprint is a weaker,
 * more false-positive-prone witness for a key.
 *
 * Since the void bit dominates the value of a slot, ordering the slots of a
 * run by their raw value orders them by fingerprint length first and by
 * fingerprint value second. That is the invariant this table maintains, and it
 * is what makes "find the longest fingerprint matching this key" a single
 * left-to-right scan of a run that keeps the *last* match it sees.
 *
 * Expanding shortens every fingerprint by one bit. A fingerprint that reaches
 * length zero ("a void entry") no longer has a bit to donate, so on the next
 * expansion the item could belong to either of the two candidate buckets and
 * is stored in *both* of them. Contracting is the exact inverse of expanding:
 * the bucket index gives the bit back, and fingerprints grow by one bit, up to
 * the width of a slot. It is an exact inverse for every entry that still has a
 * fingerprint left; the copies of a void entry are *not* merged back into one,
 * as there is no way to tell them apart from two distinct items that happen to
 * collide. So a table that has been expanded past the point where its
 * fingerprints run out holds more entries than it started with, both after
 * expanding and after contracting back, and `Contract` can fail outright if
 * those copies no longer fit.
 *
 * ---------------------------------------------------------------------------
 * Stretching (Zeno filter, Section 4.1)
 * ---------------------------------------------------------------------------
 * Donating a fingerprint bit to the bucket index doubles the table, which
 * leaves it holding as much as `2 / max_load_factor` times the space it needs.
 * The growth coefficient `r` buys that back: the table grows by a factor of
 * `2^(1/r)` per expansion instead, so it takes `r` expansions -- a *period* --
 * to double, and only the last of them, the one that lands the table back on a
 * power-of-two multiple of its original size, sacrifices a fingerprint bit.
 * `r = 1` is plain doubling, and is the default.
 *
 * The `r - 1` intermediate expansions, the *epochs*, are pure Stretching: the
 * bucket index stays just as wide, and a run simply moves further out. Writing
 * `base` for a bucket of the address space the period started with, expanding
 * to epoch `e` places that run at
 *
 *      stretch(base) = floor(base * 2^(e / r))
 *
 * and sizes the table at `stretch(base_nslots)`. That map is strictly
 * increasing, which is all the RSQF asks of it -- runs keep their relative
 * order -- and being strictly increasing it is invertible on the buckets that
 * are actually occupied, which is how a rebuild recovers an entry's hash from
 * the slot it sits in.
 *
 * Stretching does not split runs, it only spaces them out, so it cannot be
 * used on its own: run lengths would grow without bound and take the false
 * positive rate and the query cost with them. The fingerprint sacrifice ending
 * each period is what keeps them in check.
 */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "VALECounters.hpp"
#include "util.hpp"

namespace sublime {

/******************************************************************
 * Hash functions, taken verbatim from Memento filter so that the *
 * hashing semantics of the two structures agree.                 *
 ******************************************************************/

/**
 * MurmurHash2, 64-bit version, by Austin Appleby.
 */
inline uint64_t MurmurHash64A(const void *key, int32_t len, uint32_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995;
    const int r = 47;

    uint64_t h = seed ^ (len * m);

    const uint64_t *data = (const uint64_t *) key;
    const uint64_t *end = data + (len / 8);

    while (data != end) {
        uint64_t k = *data++;

        k *= m;
        k ^= k >> r;
        k *= m;

        h ^= k;
        h *= m;
    }

    const unsigned char *data2 = (const unsigned char *) data;

    switch (len & 7) {
        case 7: h ^= (uint64_t) data2[6] << 48;  /* fallthrough */
        case 6: h ^= (uint64_t) data2[5] << 40;  /* fallthrough */
        case 5: h ^= (uint64_t) data2[4] << 32;  /* fallthrough */
        case 4: h ^= (uint64_t) data2[3] << 24;  /* fallthrough */
        case 3: h ^= (uint64_t) data2[2] << 16;  /* fallthrough */
        case 2: h ^= (uint64_t) data2[1] << 8;   /* fallthrough */
        case 1: h ^= (uint64_t) data2[0];
                h *= m;
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

/** Thomas Wang's integer hash function. */
inline uint64_t hash_64(uint64_t key, uint64_t mask) {
    key = (~key + (key << 21)) & mask;
    key = key ^ key >> 24;
    key = ((key + (key << 3)) + (key << 8)) & mask;
    key = key ^ key >> 14;
    key = ((key + (key << 2)) + (key << 4)) & mask;
    key = key ^ key >> 28;
    key = (key + (key << 31)) & mask;
    return key;
}


/******************************************************************
 * Bit manipulation helpers. The ones that `util.hpp` already      *
 * provides with matching semantics are reused as is.             *
 ******************************************************************/

namespace fpt {

/** `BITMASK` from `util.hpp` as a function, to keep macros out of this file. */
__attribute__((always_inline))
static constexpr inline uint64_t bitmask(uint32_t nbits) {
    return nbits >= 64 ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << nbits) - 1);
}

/**
 * @returns The number of set bits in `val` up to *and including* position
 * `pos`. Note that `util.hpp`'s `bit_rank` excludes `pos`.
 */
__attribute__((always_inline))
static inline uint32_t bitrank_inclusive(uint64_t val, uint32_t pos) {
    return __builtin_popcountll(val & ((2ULL << pos) - 1));
}

/** @returns The number of set bits in `val`, ignoring its `ignore` lowest bits. */
__attribute__((always_inline))
static inline uint32_t popcnt_ignore(uint64_t val, uint32_t ignore) {
    return __builtin_popcountll(val & ~bitmask(ignore % 64));
}

/**
 * @returns The position of the `rank`-th set bit of `val`, ignoring its
 * `ignore` lowest bits. Returns 64 if there is no such bit.
 */
__attribute__((always_inline))
static inline uint32_t bitselect_ignore(uint64_t val, uint32_t ignore, uint32_t rank) {
    return bit_select(val & ~bitmask(ignore % 64), rank);
}

/**
 * @returns The position of the highest set bit of `val`, or 64 if `val` is 0.
 * Unlike `util.hpp`'s `highbit_pos`, this is well defined for `val == 0`.
 */
__attribute__((always_inline))
static inline uint32_t highbit_position(uint64_t val) {
    return val == 0 ? 64 : 63 - __builtin_clzll(val);
}

/** @returns The position of the lowest set bit of `val`, or 64 if `val` is 0. */
__attribute__((always_inline))
static inline uint32_t lowbit_position(uint64_t val) {
    return val == 0 ? 64 : __builtin_ctzll(val);
}

/**
 * Shifts the bits of `b` in the range [`bstart`, `bend`) up by `amount`,
 * shifting the top `amount` bits of `a` into the vacated space.
 */
__attribute__((always_inline))
static inline uint64_t shift_into_b(const uint64_t a, const uint64_t b,
                                    const int bstart, const int bend,
                                    const int amount) {
    const uint64_t a_component = bstart == 0 ? (a >> (64 - amount)) : 0;
    const uint64_t b_shifted_mask = bitmask(bend - bstart) << bstart;
    const uint64_t b_shifted = ((b_shifted_mask & b) << amount) & b_shifted_mask;
    const uint64_t b_mask = ~b_shifted_mask;
    return a_component | b_shifted | (b & b_mask);
}

}   // namespace fpt


/**
 * A compact hash table holding a multiset of variable-length fingerprints,
 * built on a rank-and-select quotient filter.
 *
 * Inserting a key appends a *full-length* fingerprint for it to the key's run.
 * Deleting a key removes the *longest* stored fingerprint in that run that
 * matches the key's hash. Both the table's slot count and the length of the
 * stored fingerprints adapt through `Expand` and `Contract`.
 */
class FingerprintTable {
    friend class FingerprintTableTest;
    friend class SublimeMGTest;

public:
    /**
     * The table supports the same three hashing modes as Memento filter:
     *
     *   - `Default` hashes keys with MurmurHash, which may introduce false
     *     positives but accepts keys of any size.
     *
     *   - `Invertible` uses an invertible integer hash, so the hash is as wide
     *     as the key.
     *
     *   - `None` uses the key as its own hash. Useful for testing, and for
     *     callers that have already hashed their keys. Beware that a skewed
     *     input distribution translates directly into skewed load here.
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

    /** The load factor at which `Insert` expands, if auto-expansion is on. */
    static constexpr double max_load_factor = 0.95;

private:
    /** Must be 64: the rank/select routines operate on single 64-bit words. */
    static constexpr uint32_t block_offset_bits_ = 6;
    static constexpr uint32_t slots_per_block_ = 1ULL << block_offset_bits_;
    static_assert(slots_per_block_ == 64, "The metadata routines assume one 64-bit word per block.");

    /**
     * A block of `slots_per_block_` slots, along with the metadata that makes
     * rank-and-select queries over the table local to a block.
     */
    struct __attribute__ ((__packed__)) Block {
        uint8_t offset;         /**< Distance from this block's first slot to the end of the run it belongs to. */
        uint64_t occupieds;     /**< Bit `i` is set iff slot `i` of this block is some run's canonical slot. */
        uint64_t runends;       /**< Bit `i` is set iff slot `i` of this block is the last slot of a run. */
        uint8_t slots[1];       /**< The slots themselves, packed `bits_per_slot_` bits each. */
    };

public:
    /**
     * Constructs an empty table.
     *
     * @param nslots The number of slots. Need not be a power of two.
     * @param key_bits The number of bits of the hash the table uses. The
     * fingerprint length is derived as `key_bits - ceil(log2(nslots))`.
     * @param hash_mode The hashing mode, see `hashmode`.
     * @param seed The seed of the hash function.
     * @param growth_coefficient `r`, the growth coefficient of Zeno filter's
     * Stretching: `Expand` grows the table by a factor of `2^(1/r)`, so that
     * it takes `r` expansions to double the table and to shed a fingerprint
     * bit. Must be at least 1, which is plain doubling.
     * @param orig_quotient_bit_cnt The number of low hash bits fast-reduced
     * into the original slot count. Defaults to the table's own quotient
     * width, which is what a freshly constructed table wants. `Expand` and
     * `Contract` propagate the original value so that a key keeps hashing to
     * the same place as the table changes size.
     */
    explicit FingerprintTable(uint64_t nslots, uint64_t key_bits, hashmode hash_mode,
                              uint32_t seed, uint32_t growth_coefficient = 1,
                              uint64_t orig_quotient_bit_cnt = 0);
    ~FingerprintTable();
    FingerprintTable(const FingerprintTable& other);
    FingerprintTable(FingerprintTable&& other) noexcept;
    FingerprintTable& operator=(const FingerprintTable& other);
    FingerprintTable& operator=(FingerprintTable&& other) noexcept;

    /**
     * Inserts a full-length fingerprint for `key`. Repeated insertions of the
     * same key store repeated fingerprints, i.e., the table is a multiset.
     *
     * @param key The key to insert.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns 0 on success, or `err_no_space` if the table is full and
     * auto-expansion is disabled.
     */
    int32_t Insert(uint64_t key, uint8_t flags = 0);

    /**
     * As `Insert`, but reporting the slot the new fingerprint landed in, which
     * is where a caller keeping counts has to write the new entry's count.
     *
     * @returns The slot, or a negative status code.
     */
    int64_t InsertAt(uint64_t key, uint8_t flags = 0);

    /**
     * Removes the longest stored fingerprint matching `key`'s hash.
     *
     * @param key The key to delete.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns 0 on success, or `err_doesnt_exist` if nothing in `key`'s run
     * matches it.
     */
    int32_t Delete(uint64_t key, uint8_t flags = 0);

    /**
     * Removes the entry sitting in `slot`, whose run belongs to `bucket`. Both
     * normally come from a scan that has already located the entry, such as
     * iteration. Removing an entry addressed this way, rather than by key,
     * matters when several entries of a run match the same key and the caller
     * has picked out a particular one -- as an eviction policy does.
     *
     * Removing a slot slides the rest of its cluster down by one, so slots
     * *above* it move and slots below it do not. A caller removing several
     * entries it located in one pass should therefore work from the highest
     * slot down, and the slots it has yet to reach stay where it found them.
     */
    void DeleteSlot(uint64_t bucket, uint64_t slot);

    /** One entry of a run, and the sum its chain of prefixes adds up to. */
    struct ChainedEntry {
        uint64_t slot;          /**< The slot it sits in, and its counter's index. */
        uint64_t bucket;        /**< The canonical slot of its run. */
        uint64_t fingerprint;   /**< The raw slot contents: void bit and fingerprint. */
        uint64_t count;         /**< Its own counter. */
        /**
         * Its counter plus the counters of every earlier entry of its run
         * whose fingerprint is a prefix of its own -- which is exactly what a
         * query matching it would add up.
         */
        uint64_t chain_sum;
    };

    /**
     * Calls `f(entry)` for every stored fingerprint, run by run and in slot
     * order, handing it the entry's chain sum along with it.
     *
     * A run is held in ascending slot value, and the void bit outweighs the
     * fingerprint, so a run is in ascending order of *length*: an earlier
     * entry of a run is a prefix of a later one exactly when the two match.
     * The chain sums therefore come out of one left-to-right sweep per run.
     *
     * Requires counters. `f` must not change the table's shape; it may write
     * counters, since each run is read out in full before any of its entries
     * are reported and no later run has been read yet.
     */
    template <typename F>
    void ForEachEntryWithChainSum(F&& f) const;

    /**
     * Removes the given entries and repairs the runs they leave behind, so
     * that every entry still standing keeps the chain sum it had.
     *
     * Removing an entry takes its counter out of the chain sums of every
     * longer entry of its run that matched it, which would silently cut those
     * entries' counts. The repair puts it back: the entries that lose their
     * last surviving prefix take on what the entries removed from under them
     * held. That is one addition per newly rootless entry, and nothing else in
     * the run is touched -- in particular no counter is ever made smaller, so
     * no count can go negative here.
     *
     * The entries to remove must be *downward closed* within each run: if an
     * entry goes, every entry of its run that is a prefix of it goes too.
     * Anything an eviction policy driven by chain sums picks out has that
     * property already, since a prefix's chain sum is no larger than the sums
     * of the entries above it.
     *
     * @param victims The entries to remove, in any order. Left reordered.
     * @returns The change in the total of all the counters, which is negative
     * unless a removed entry's counter had to be carried over into more than
     * one entry above it.
     */
    int64_t DeleteEntriesPreservingChainSums(std::vector<std::pair<uint64_t, uint64_t>>& victims);

    /**
     * Removes the longest stored fingerprint matching `key`, repairing its run
     * exactly as `DeleteEntriesPreservingChainSums` does. The longest match of
     * one key can still be a prefix of entries that match other keys, so the
     * repair is needed here too.
     *
     * @param counter_delta Set to the change in the total of all the counters.
     * @returns 0, or `err_doesnt_exist` if nothing in `key`'s run matches it.
     */
    int32_t DeletePreservingChainSums(uint64_t key, uint8_t flags, int64_t& counter_delta);

    /**
     * @returns The bits of `key`'s hash that the table keeps: its bucket and
     * its full-length fingerprint. Two keys share this value exactly when no
     * insertion or query can tell them apart, which is what a caller batching
     * insertions up needs in order to merge them.
     */
    uint64_t EntryIdentity(uint64_t key, uint8_t flags = 0) const {
        return hash_key(key, flags) & fpt::bitmask(key_bits_);
    }

    /**
     * @param key The key to look up.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns The number of stored fingerprints matching `key`'s hash. This
     * never undercounts how many times `key` was inserted, but it may
     * overcount, as a fingerprint of another key may match by chance.
     */
    uint64_t Count(uint64_t key, uint8_t flags = 0) const;

    /**
     * @param key The key to look up.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns `true` if any stored fingerprint matches `key`'s hash.
     */
    bool Contains(uint64_t key, uint8_t flags = 0) const {
        return MatchLength(key, flags) >= 0;
    }

    /**
     * @param key The key to look up.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns The length, in bits, of the longest stored fingerprint matching
     * `key`'s hash, or -1 if nothing matches it.
     */
    int32_t MatchLength(uint64_t key, uint8_t flags = 0) const;

    /**
     * @returns The slot holding the longest stored fingerprint matching `key`,
     * or -1 if the key's run holds no match at all. That slot is where a
     * mirroring counter array keeps the fingerprint's count, and the search
     * starts that counter's chunk on its way into cache before it touches the
     * table at all, so that reading or updating the count does not stall on a
     * second miss queued up behind this one.
     */
    int64_t FindLongestMatch(uint64_t key, uint8_t flags = 0) const;

    /** What `SumMatchingCounters` found in a key's run. */
    struct MatchSum {
        uint64_t count = 0;     /**< How many stored fingerprints matched. */
        uint64_t sum = 0;       /**< The sum of their counters. */
    };

    /**
     * Scans `key`'s run once and adds up the counters of *every* stored
     * fingerprint matching it, of whatever length. A matching fingerprint
     * shorter than the full length may belong to another key, so its counter
     * is not necessarily the key's -- but the key's own counter is certainly
     * among those added up, which is what makes the total an over-estimate
     * rather than a possible under-estimate.
     *
     * @param key The key to look up.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns The number of matches and the sum of their counters; `{0, 0}`
     * if nothing matches. All zeroes, too, if the table has no counters.
     */
    MatchSum SumMatchingCounters(uint64_t key, uint8_t flags = 0) const;

    /**
     * Doubles the number of slots. Every stored fingerprint donates its lowest
     * bit to the bucket index and so loses one bit of length. A fingerprint
     * that is already of length zero has no bit to donate, so its item is
     * stored in both of the buckets it could now belong to.
     *
     * Counts, if the table is keeping any, follow their fingerprints across.
     * Both copies of a duplicated void entry keep the whole count: neither has
     * a fingerprint left to tell it from the other, so a query may land on
     * either, and giving each the full count is what keeps the answer an
     * over-estimate rather than an under-estimate.
     *
     * @returns The number of fingerprints in the expanded table, or a negative
     * status code on failure. The table is left untouched on failure.
     */
    int64_t Expand();

    /**
     * Halves the number of slots, the inverse of `Expand`: the bucket index
     * gives a bit back to every fingerprint, which therefore grows by one bit,
     * capped at the width of a slot. The duplicate copies of a void entry are
     * not merged back into one, so contracting a table that was expanded past
     * the length of its fingerprints does not undo the growth in entry count.
     *
     * @returns The number of fingerprints in the contracted table,
     * `err_cannot_contract` if the table is already at its original size, or
     * `err_no_space` if the entries do not fit in the smaller table. The table
     * is left untouched on failure.
     */
    int64_t Contract();

    /** Empties the table without changing its size. */
    void Reset();

    /**
     * Controls whether `Insert` expands the table on its own once it reaches
     * `max_load_factor`. Off by default, as Sublime drives expansion itself.
     */
    void SetAutoExpand(bool enabled) {
        auto_expand_ = enabled;
    }
    bool IsAutoExpandEnabled() const {
        return auto_expand_;
    }

    /* Size and shape of the table. */
    uint64_t SizeInBytes() const {
        return buffer_size_;
    }
    uint64_t CountSlots() const {
        return nslots_;
    }
    uint64_t CountOccupiedSlots() const {
        return noccupied_slots_;
    }
    /** @returns The number of fingerprints stored, one per occupied slot. */
    uint64_t CountFingerprints() const {
        return noccupied_slots_;
    }
    double LoadFactor() const {
        return static_cast<double>(noccupied_slots_) / nslots_;
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
        return key_bits_ - fingerprint_bits_;
    }
    uint64_t GetOriginalQuotientBits() const {
        return original_quotient_bits_;
    }
    /** @returns The slot count the table was constructed with. */
    uint64_t GetOriginalSlotCount() const {
        return original_nslots_;
    }
    /**
     * @returns The number of slots actually allocated. Larger than
     * `CountSlots` by the room left for runs to spill past the last bucket,
     * and the number of counters a mirroring array needs.
     */
    uint64_t GetSlotCapacity() const {
        return xnslots_;
    }

    /**
     * Gives the table a counter per slot, all starting at zero, and has it
     * mirror every slot movement into them: counter `i` then follows the
     * fingerprint in slot `i` wherever insertions, deletions, expansions, and
     * contractions push it.
     *
     * The table owns the array, because the two have to be resized in lockstep
     * and only the table knows when that happens. Calling this again zeroes
     * the counters and re-shapes them to the current slot capacity.
     */
    void EnableCounters() {
        counters_ = std::make_unique<VALECounters>(xnslots_);
    }
    /** Throws the counters away, leaving a plain fingerprint table. */
    void DisableCounters() {
        counters_.reset();
    }
    bool CountersEnabled() const {
        return counters_ != nullptr;
    }
    /** @returns The counters, or null if none were enabled. */
    VALECounters *GetCounters() {
        return counters_.get();
    }
    const VALECounters *GetCounters() const {
        return counters_.get();
    }

    /* Stretching. */

    /** @returns `r`: every expansion grows the table by a factor of `2^(1/r)`. */
    uint32_t GetGrowthCoefficient() const {
        return growth_coefficient_;
    }
    /**
     * @returns How many expansions the table has made within the current
     * period, in `[0, r)`. Zero means the table sits on a power-of-two
     * multiple of its original size, with nothing stretched.
     */
    uint32_t GetEpoch() const {
        return epoch_;
    }
    /**
     * @returns How many periods the table has completed, i.e. how many times
     * it has doubled and taken a bit off every fingerprint.
     */
    uint64_t GetPeriodCount() const {
        return GetBucketIndexHashSize() - original_quotient_bits_;
    }
    /**
     * @returns The slot count the current period started with, which is the
     * address space `Expand` stretches over the table's actual slots.
     */
    uint64_t GetBaseSlotCount() const {
        return base_nslots_;
    }

    /** @returns How many times the table has been expanded past its original size. */
    uint64_t GetExpansionCount() const {
        return GetPeriodCount() * growth_coefficient_ + epoch_;
    }

    /**
     * The slot count `Expand` would leave behind, worked out from the shape
     * arithmetic alone rather than by building the table. Lets a caller that
     * drives expansion from a size function -- Sublime_MG -- price the next
     * expansion before committing to it.
     *
     * @returns The number of slots after one expansion, or the current count
     * if the table cannot expand any further.
     */
    uint64_t CountSlotsAfterExpansion() const {
        const bool ending_period = (epoch_ + 1 >= growth_coefficient_);
        if (ending_period && key_bits_ + 2 > 64)
            return nslots_;
        return ending_period ? base_nslots_ * 2
                             : stretch_at(base_nslots_, epoch_ + 1, growth_coefficient_);
    }

    /**
     * The mirror of `CountSlotsAfterExpansion`.
     *
     * @returns The number of slots after one contraction, or the current count
     * if the table is already at its original size.
     */
    uint64_t CountSlotsAfterContraction() const {
        if (GetExpansionCount() == 0)
            return nslots_;
        return epoch_ == 0 ? stretch_at(base_nslots_ / 2, growth_coefficient_ - 1, growth_coefficient_)
                           : stretch_at(base_nslots_, epoch_ - 1, growth_coefficient_);
    }
    hashmode GetHashMode() const {
        return hash_mode_;
    }
    uint32_t GetHashSeed() const {
        return seed_;
    }

    void DebugDumpBlock(uint64_t i) const;
    void DebugDumpMetadata() const;
    void DebugDump() const {
        DebugDumpMetadata();
        for (uint64_t i = 0; i < nblocks_; i++)
            DebugDumpBlock(i);
    }

    /**
     * A forward iterator over every fingerprint in the table, walking runs in
     * order of their canonical slot.
     */
    class const_iterator {
        friend class FingerprintTable;

    public:
        const_iterator(const FingerprintTable& table): table_{&table} {}

        const_iterator& operator++();
        const_iterator operator++(int) {
            auto old = *this;
            ++(*this);
            return old;
        }

        bool operator==(const const_iterator& rhs) const {
            return current_ == rhs.current_ && run_ == rhs.run_;
        }
        bool operator!=(const const_iterator& rhs) const {
            return !(*this == rhs);
        }

        /** @returns The canonical slot of the run the iterator is in. */
        uint64_t bucket() const {
            return run_;
        }
        /**
         * @returns The slot the entry sits in, which is also the index of its
         * counter in a mirroring array.
         */
        uint64_t slot() const {
            return current_;
        }
        /** @returns The raw slot contents, i.e., the void bit and fingerprint. */
        uint64_t fingerprint() const {
            return table_->get_slot(current_);
        }
        /** @returns The length, in bits, of the fingerprint pointed at. */
        uint32_t fingerprint_length() const {
            return fpt::highbit_position(fingerprint());
        }
        /**
         * @returns As much of the hash of the item pointed at as the table
         * still remembers, laid out the way `Insert` expects a pre-hashed key.
         */
        uint64_t hash() const;

    private:
        static constexpr uint64_t end_marker_ = 0xFFFFFFFFFFFFFFFF;

        const FingerprintTable *table_;
        uint64_t run_ = 0;      /**< The canonical slot of the current run. */
        uint64_t current_ = 0;  /**< The slot the iterator points at. */
    };

    const_iterator begin() const;
    const_iterator end() const;

private:
    uint64_t nslots_;                   /**< The number of slots the table nominally holds. */
    uint64_t xnslots_;                  /**< The number of slots actually allocated, leaving room for runs to spill over. */
    uint64_t nblocks_;
    uint64_t key_bits_;
    uint64_t fingerprint_bits_;         /**< The maximum fingerprint length, i.e., the length of a freshly inserted one. */
    uint64_t bits_per_slot_;            /**< `fingerprint_bits_ + 1`, the extra bit being the void bit. */
    uint64_t original_quotient_bits_;
    uint64_t original_nslots_;          /**< The slot count the table was constructed with, before any expansion. */
    uint64_t base_nslots_;              /**< The slot count the current period started with, `original_nslots_ << GetPeriodCount()`. */
    uint32_t growth_coefficient_;       /**< `r`: an expansion grows the table by a factor of `2^(1/r)`. */
    uint32_t epoch_;                    /**< Expansions completed within the current period, in `[0, r)`. */
    uint64_t stretch_multiplier_;       /**< `2^(epoch_ / r)`, in Q32 fixed point. */
    uint64_t block_stride_;             /**< The size of a `Block`, in bytes, including its slots. */
    uint64_t noccupied_slots_ = 0;
    hashmode hash_mode_;
    uint32_t seed_;
    bool auto_expand_ = false;
    /** One count per slot, mirroring them. Null unless `EnableCounters` was called. */
    std::unique_ptr<VALECounters> counters_;
    uint8_t *buffer_ = nullptr;         /**< The blocks. */
    uint64_t buffer_size_ = 0;

    /** Everything `Expand` and `Contract` have to hand the resized table. */
    struct Shape {
        uint64_t original_nslots;       /**< Carried over unchanged; the fast reduction in `hash_key` needs it. */
        uint64_t base_nslots;           /**< The address space the new table's period starts from. */
        uint64_t key_bits;
        uint64_t orig_quotient_bits;
        uint32_t epoch;
    };

    /** Builds a table of the given shape. Used by `Expand` and `Contract`. */
    FingerprintTable(const Shape& shape, hashmode hash_mode, uint32_t seed,
                     uint32_t growth_coefficient);

    /**
     * The fingerprint gets whatever a `key_bits`-wide hash has left over the
     * `ceil(log2(nslots))`-bit quotient. Mirrors Memento filter, so that a
     * key's bucket and fingerprint agree between the two structures.
     */
    static uint64_t compute_fingerprint_bits(uint64_t nslots, uint64_t key_bits);

    void allocate(const Shape& shape, uint32_t growth_coefficient);

    /** @returns `2^(epoch / r)` in Q32 fixed point, the factor `stretch` scales by. */
    static uint64_t compute_stretch_multiplier(uint32_t epoch, uint32_t growth_coefficient) {
        if (epoch == 0)
            return 1ULL << 32;
        return static_cast<uint64_t>(std::llround(
                    std::exp2(static_cast<double>(epoch) / growth_coefficient) * 4294967296.0));
    }

    /**
     * `stretch` for a shape the table is not in, so that the slot count of a
     * neighbouring epoch can be computed without allocating one.
     */
    static uint64_t stretch_at(uint64_t base_slot, uint32_t epoch, uint32_t growth_coefficient) {
        if (epoch == 0)
            return base_slot;
        return static_cast<uint64_t>((static_cast<__uint128_t>(base_slot)
                    * compute_stretch_multiplier(epoch, growth_coefficient)) >> 32);
    }

    /**
     * Maps a slot of the period's base address space to its slot in the
     * current, stretched one: `floor(base_slot * 2^(epoch / r))`, evaluated in
     * Q32 fixed point so that the answer is exact and reproducible rather than
     * at the mercy of the platform's floating point.
     *
     * Multiplication by a constant `>= 1` followed by a floor is *strictly*
     * increasing, which is the property the whole scheme rests on: distinct
     * base buckets stay distinct and stay in order, so runs never cross and
     * `unstretch` can undo this.
     */
    uint64_t stretch(uint64_t base_slot) const {
        if (epoch_ == 0)
            return base_slot;
        return static_cast<uint64_t>((static_cast<__uint128_t>(base_slot)
                                        * stretch_multiplier_) >> 32);
    }

    /**
     * The inverse of `stretch`. Only defined on that map's image, which is
     * where every canonical slot of the table lives; the slots in between are
     * the empty space Stretching opens up and belong to no bucket at all.
     */
    uint64_t unstretch(uint64_t slot) const {
        if (epoch_ == 0)
            return slot;
        // The least `b` with `floor(b * m / 2^32) >= slot` is `ceil(slot * 2^32 / m)`.
        const __uint128_t numerator = (static_cast<__uint128_t>(slot) << 32)
                                        + stretch_multiplier_ - 1;
        const uint64_t base_slot = static_cast<uint64_t>(numerator / stretch_multiplier_);
        assert(stretch(base_slot) == slot);
        return base_slot;
    }

    Block *get_block(uint64_t block_index) const {
        return reinterpret_cast<Block *>(buffer_ + block_index * block_stride_);
    }

    /*
     * Since a block holds exactly 64 slots, each metadata bitmap is a single
     * word. `Block` is packed, so its words are not naturally aligned and a
     * reference cannot be bound to them; these accessors pass them by value.
     */
    uint64_t get_occupieds_word(uint64_t slot_index) const {
        return get_block(slot_index / slots_per_block_)->occupieds;
    }
    uint64_t get_runends_word(uint64_t slot_index) const {
        return get_block(slot_index / slots_per_block_)->runends;
    }
    void set_runends_word(uint64_t slot_index, uint64_t value) {
        get_block(slot_index / slots_per_block_)->runends = value;
    }
    /** @returns A pointer to the `i`-th 64-bit word of the slot payloads. */
    uint64_t *remainder_word(uint64_t i) const {
        return reinterpret_cast<uint64_t *>(&get_block(i / bits_per_slot_)->slots[8 * (i % bits_per_slot_)]);
    }

    bool is_occupied(uint64_t index) const {
        return (get_occupieds_word(index) >> (index % slots_per_block_)) & 1ULL;
    }
    bool is_runend(uint64_t index) const {
        return (get_runends_word(index) >> (index % slots_per_block_)) & 1ULL;
    }
    void set_occupied(uint64_t index) {
        Block *b = get_block(index / slots_per_block_);
        b->occupieds |= 1ULL << (index % slots_per_block_);
    }
    void clear_occupied(uint64_t index) {
        Block *b = get_block(index / slots_per_block_);
        b->occupieds &= ~(1ULL << (index % slots_per_block_));
    }
    void set_runend(uint64_t index) {
        Block *b = get_block(index / slots_per_block_);
        b->runends |= 1ULL << (index % slots_per_block_);
    }
    void clear_runend(uint64_t index) {
        Block *b = get_block(index / slots_per_block_);
        b->runends &= ~(1ULL << (index % slots_per_block_));
    }
    void flip_runend(uint64_t index) {
        Block *b = get_block(index / slots_per_block_);
        b->runends ^= 1ULL << (index % slots_per_block_);
    }

    uint64_t get_slot(uint64_t index) const;
    void set_slot(uint64_t index, uint64_t value);

    uint64_t block_offset(uint64_t block_index) const;
    uint64_t run_end(uint64_t bucket_index) const;
    int32_t offset_lower_bound(uint64_t slot_index) const;
    uint64_t find_first_empty_slot(uint64_t from) const;
    uint64_t run_start(uint64_t bucket_index) const {
        return bucket_index == 0 ? 0 : run_end(bucket_index - 1) + 1;
    }

    void shift_remainders(uint64_t start_index, uint64_t empty_index);
    void shift_slots(int64_t first, uint64_t last, uint64_t distance);
    void shift_runends(int64_t first, uint64_t last, uint64_t distance);
    void remove_slot(bool only_item_in_run, uint64_t bucket_index, uint64_t remove_index);

    /**
     * Gives this table an empty counter array to receive `source`'s counts, if
     * `source` is keeping any. It is shaped the way `source`'s array is rather
     * than freshly tuned, which both skips an immediate retune and makes sure
     * every count `source` holds fits.
     */
    void mirror_counters_of(const FingerprintTable& source) {
        if (source.counters_ != nullptr)
            counters_ = std::make_unique<VALECounters>(xnslots_, *source.counters_);
    }

    /** Writes a count into the slot `slot`, if the table is keeping counts at all. */
    void set_count(uint64_t slot, uint64_t count) {
        if (counters_ != nullptr)
            counters_->Set(slot, count);
    }

    /** Folds a key into the canonical hash form described at the top of this file. */
    uint64_t hash_key(uint64_t key, uint8_t flags) const;

    uint64_t bucket_from_hash(uint64_t hash) const {
        const uint32_t bihs = GetBucketIndexHashSize();
        const uint32_t oqs = original_quotient_bits_;
        const uint64_t base_bucket = ((hash & fpt::bitmask(oqs)) << (bihs - oqs))
                                   | ((hash >> oqs) & fpt::bitmask(bihs - oqs));
        return stretch(base_bucket);
    }

    /** @returns The slot contents encoding a length-`len` fingerprint of `hash`. */
    uint64_t fingerprint_from_hash(uint64_t hash, uint32_t len) const {
        return ((hash >> GetBucketIndexHashSize()) & fpt::bitmask(len)) | (1ULL << len);
    }

    /**
     * @returns Whether `stored` is a prefix match of the full-length
     * fingerprint `target`, i.e., whether the two agree on `stored`'s bits.
     */
    static bool fingerprints_match(uint64_t stored, uint64_t target) {
        return ((stored ^ target) & fpt::bitmask(fpt::highbit_position(stored))) == 0;
    }

    /**
     * @param runstart The first slot of the run to scan.
     * @param fingerprint The full-length fingerprint to match against.
     * @returns The position of the longest fingerprint in the run matching
     * `fingerprint`, or -1 if there is none.
     */
    int64_t longest_match_in_run(uint64_t runstart, uint64_t fingerprint) const;

    /**
     * Reads out every entry of `bucket`'s run, in slot order, leaving each
     * one's `chain_sum` unset. `out` is cleared first.
     */
    void collect_run(uint64_t bucket, std::vector<ChainedEntry>& out) const;

    /**
     * @param runstart The first slot of the run to scan.
     * @param fingerprint The fingerprint to place.
     * @returns The first slot of the run holding a value strictly greater than
     * `fingerprint`, or one past the run's last slot if there is none. New
     * fingerprints go here, which keeps every run sorted.
     */
    uint64_t upper_bound_in_run(uint64_t runstart, uint64_t fingerprint) const;

    /** Inserts `fingerprint` into `bucket_index`'s run, keeping it sorted. */
    /**
     * @returns The slot the fingerprint ended up in, or a negative status
     * code. The slot is what a caller carrying a count over to it needs.
     */
    int64_t insert_fingerprint(uint64_t bucket_index, uint64_t fingerprint);

    /** Inserts a length-`len` fingerprint of the pre-folded `hash`. */
    int32_t insert_hash(uint64_t hash, uint32_t len) {
        const int64_t ret = insert_hash_at(hash, len);
        return ret < 0 ? static_cast<int32_t>(ret) : 0;
    }

    /** As `insert_hash`, but reporting the slot the fingerprint landed in. */
    int64_t insert_hash_at(uint64_t hash, uint32_t len) {
        return insert_fingerprint(bucket_from_hash(hash), fingerprint_from_hash(hash, len));
    }

    /** Rebuilds this table into `dest`, whose size differs by a factor of two. */
    /** What a rebuild has to do to each entry's fingerprint on the way over. */
    enum class rebuild_op {
        restretch,      /**< Nothing: the bucket index is as wide as it was, only stretched differently. */
        sacrifice,      /**< Every fingerprint donates its lowest bit to the bucket index. */
        unsacrifice     /**< Every fingerprint takes a bit back from the bucket index. */
    };

    int64_t rebuild_into(FingerprintTable& dest, rebuild_op op) const;
};


inline uint64_t FingerprintTable::compute_fingerprint_bits(uint64_t nslots, uint64_t key_bits) {
    const uint64_t num_slots = nslots;
    uint64_t fp_bits = key_bits;
    while (nslots > 1) {
        assert(fp_bits > 0);
        fp_bits--;
        nslots >>= 1;
    }
    fp_bits -= (__builtin_popcountll(num_slots) > 1);
    return fp_bits;
}


inline void FingerprintTable::allocate(const Shape& shape, uint32_t growth_coefficient) {
    assert(growth_coefficient >= 1);
    assert(shape.epoch < growth_coefficient);
    growth_coefficient_ = growth_coefficient;
    epoch_ = shape.epoch;
    stretch_multiplier_ = compute_stretch_multiplier(epoch_, growth_coefficient_);

    original_nslots_ = shape.original_nslots;
    base_nslots_ = shape.base_nslots;
    key_bits_ = shape.key_bits;
    // Stretching hands out no hash bits, so the fingerprint is sized against
    // the address space the period started with, not the stretched one.
    fingerprint_bits_ = compute_fingerprint_bits(base_nslots_, key_bits_);
    // ... and the slots those buckets are spread over are what we allocate.
    nslots_ = stretch(base_nslots_);
    xnslots_ = nslots_ + 10 * sqrt((double) nslots_);
    nblocks_ = (xnslots_ + slots_per_block_ - 1) / slots_per_block_;

    // The extra bit is the void bit delimiting the fingerprint's length.
    bits_per_slot_ = fingerprint_bits_ + 1;
    original_quotient_bits_ = shape.orig_quotient_bits ? shape.orig_quotient_bits
                                                       : key_bits_ - fingerprint_bits_;

    // A slot must be addressable by the 64-bit reads in `get_slot`, and a
    // reconstructed hash carries the quotient, the fingerprint, and the void
    // bit, so it must fit in a word too.
    assert(bits_per_slot_ <= 56);
    assert(key_bits_ + 1 <= 64);
    assert(original_quotient_bits_ <= 32);
    assert(GetBucketIndexHashSize() >= original_quotient_bits_);
    assert(base_nslots_ == (original_nslots_ << GetPeriodCount()));

    block_stride_ = sizeof(Block) - sizeof(Block::slots) + slots_per_block_ * bits_per_slot_ / 8;
    // `get_slot`/`set_slot` read and write whole words, so they may run up to
    // 7 bytes past the last slot. The padding keeps that in bounds.
    buffer_size_ = nblocks_ * block_stride_ + sizeof(uint64_t);
    buffer_ = new uint8_t[buffer_size_]{};
    noccupied_slots_ = 0;
}


inline FingerprintTable::FingerprintTable(uint64_t nslots, uint64_t key_bits, hashmode hash_mode,
                                          uint32_t seed, uint32_t growth_coefficient,
                                          uint64_t orig_quotient_bit_cnt):
        hash_mode_{hash_mode},
        seed_{seed} {
    // A fresh table is at epoch 0 of its first period, so nothing is stretched
    // yet and its base address space is the one it was asked for.
    allocate({nslots, nslots, key_bits, orig_quotient_bit_cnt, 0}, growth_coefficient);
}


inline FingerprintTable::FingerprintTable(const Shape& shape, hashmode hash_mode, uint32_t seed,
                                          uint32_t growth_coefficient):
        hash_mode_{hash_mode},
        seed_{seed} {
    allocate(shape, growth_coefficient);
}


inline FingerprintTable::~FingerprintTable() {
    delete[] buffer_;
}


inline FingerprintTable::FingerprintTable(const FingerprintTable& other):
        nslots_{other.nslots_},
        xnslots_{other.xnslots_},
        nblocks_{other.nblocks_},
        key_bits_{other.key_bits_},
        fingerprint_bits_{other.fingerprint_bits_},
        bits_per_slot_{other.bits_per_slot_},
        original_quotient_bits_{other.original_quotient_bits_},
        original_nslots_{other.original_nslots_},
        base_nslots_{other.base_nslots_},
        growth_coefficient_{other.growth_coefficient_},
        epoch_{other.epoch_},
        stretch_multiplier_{other.stretch_multiplier_},
        block_stride_{other.block_stride_},
        noccupied_slots_{other.noccupied_slots_},
        hash_mode_{other.hash_mode_},
        seed_{other.seed_},
        auto_expand_{other.auto_expand_},
        counters_{other.counters_ ? std::make_unique<VALECounters>(*other.counters_) : nullptr},
        buffer_size_{other.buffer_size_} {
    buffer_ = new uint8_t[buffer_size_];
    memcpy(buffer_, other.buffer_, buffer_size_);
}


inline FingerprintTable::FingerprintTable(FingerprintTable&& other) noexcept:
        nslots_{other.nslots_},
        xnslots_{other.xnslots_},
        nblocks_{other.nblocks_},
        key_bits_{other.key_bits_},
        fingerprint_bits_{other.fingerprint_bits_},
        bits_per_slot_{other.bits_per_slot_},
        original_quotient_bits_{other.original_quotient_bits_},
        original_nslots_{other.original_nslots_},
        base_nslots_{other.base_nslots_},
        growth_coefficient_{other.growth_coefficient_},
        epoch_{other.epoch_},
        stretch_multiplier_{other.stretch_multiplier_},
        block_stride_{other.block_stride_},
        noccupied_slots_{other.noccupied_slots_},
        hash_mode_{other.hash_mode_},
        seed_{other.seed_},
        auto_expand_{other.auto_expand_},
        counters_{std::move(other.counters_)},
        buffer_{other.buffer_},
        buffer_size_{other.buffer_size_} {
    other.buffer_ = nullptr;
    other.buffer_size_ = 0;
}


inline FingerprintTable& FingerprintTable::operator=(const FingerprintTable& other) {
    if (this == &other)
        return *this;
    FingerprintTable tmp(other);
    *this = std::move(tmp);
    return *this;
}


inline FingerprintTable& FingerprintTable::operator=(FingerprintTable&& other) noexcept {
    if (this == &other)
        return *this;
    delete[] buffer_;

    nslots_ = other.nslots_;
    xnslots_ = other.xnslots_;
    nblocks_ = other.nblocks_;
    key_bits_ = other.key_bits_;
    fingerprint_bits_ = other.fingerprint_bits_;
    bits_per_slot_ = other.bits_per_slot_;
    original_quotient_bits_ = other.original_quotient_bits_;
    original_nslots_ = other.original_nslots_;
    base_nslots_ = other.base_nslots_;
    growth_coefficient_ = other.growth_coefficient_;
    epoch_ = other.epoch_;
    stretch_multiplier_ = other.stretch_multiplier_;
    block_stride_ = other.block_stride_;
    noccupied_slots_ = other.noccupied_slots_;
    hash_mode_ = other.hash_mode_;
    seed_ = other.seed_;
    auto_expand_ = other.auto_expand_;
    counters_ = std::move(other.counters_);
    buffer_ = other.buffer_;
    buffer_size_ = other.buffer_size_;

    other.buffer_ = nullptr;
    other.buffer_size_ = 0;
    return *this;
}


inline void FingerprintTable::Reset() {
    noccupied_slots_ = 0;
    memset(buffer_, 0, buffer_size_);
    if (counters_ != nullptr)
        counters_->Reset();
}


/******************************************************************
 * Slot access.                                                   *
 ******************************************************************/

inline uint64_t FingerprintTable::get_slot(uint64_t index) const {
    assert(index < xnslots_);
    const uint8_t *p = &get_block(index / slots_per_block_)->slots[(index % slots_per_block_)
                                                                    * bits_per_slot_ / 8];
    uint64_t pvalue;
    memcpy(&pvalue, p, sizeof(pvalue));
    return (pvalue >> (((index % slots_per_block_) * bits_per_slot_) % 8)) & fpt::bitmask(bits_per_slot_);
}


inline void FingerprintTable::set_slot(uint64_t index, uint64_t value) {
    assert(index < xnslots_);
    uint8_t *p = &get_block(index / slots_per_block_)->slots[(index % slots_per_block_)
                                                              * bits_per_slot_ / 8];
    uint64_t t;
    memcpy(&t, p, sizeof(t));
    const int32_t shift = ((index % slots_per_block_) * bits_per_slot_) % 8;
    const uint64_t mask = fpt::bitmask(bits_per_slot_) << shift;
    t &= ~mask;
    t |= (value << shift) & mask;
    memcpy(p, &t, sizeof(t));
}


/******************************************************************
 * Rank-and-select navigation over the runs.                      *
 ******************************************************************/

inline uint64_t FingerprintTable::block_offset(uint64_t block_index) const {
    const Block *b = get_block(block_index);
    if (b->offset < fpt::bitmask(8 * sizeof(Block::offset)))
        return b->offset;
    // The offset field saturated, so recompute it the slow way.
    return run_end(slots_per_block_ * block_index - 1) - slots_per_block_ * block_index + 1;
}


inline uint64_t FingerprintTable::run_end(uint64_t bucket_index) const {
    const uint64_t bucket_block_index = bucket_index / slots_per_block_;
    const uint64_t bucket_intrablock_offset = bucket_index % slots_per_block_;
    const uint64_t bucket_blocks_offset = block_offset(bucket_block_index);

    const uint64_t bucket_intrablock_rank = fpt::bitrank_inclusive(get_block(bucket_block_index)->occupieds,
                                                                   bucket_intrablock_offset);
    if (bucket_intrablock_rank == 0) {
        if (bucket_blocks_offset <= bucket_intrablock_offset)
            return bucket_index;
        return slots_per_block_ * bucket_block_index + bucket_blocks_offset - 1;
    }

    uint64_t runend_block_index = bucket_block_index + bucket_blocks_offset / slots_per_block_;
    uint64_t runend_ignore_bits = bucket_blocks_offset % slots_per_block_;
    uint64_t runend_rank = bucket_intrablock_rank - 1;
    uint64_t runend_block_offset = fpt::bitselect_ignore(get_block(runend_block_index)->runends,
                                                         runend_ignore_bits, runend_rank);
    if (runend_block_offset == slots_per_block_) {
        do {
            runend_rank -= fpt::popcnt_ignore(get_block(runend_block_index)->runends, runend_ignore_bits);
            runend_block_index++;
            runend_ignore_bits = 0;
            runend_block_offset = fpt::bitselect_ignore(get_block(runend_block_index)->runends,
                                                        runend_ignore_bits, runend_rank);
        } while (runend_block_offset == slots_per_block_);
    }

    const uint64_t runend_index = slots_per_block_ * runend_block_index + runend_block_offset;
    return runend_index < bucket_index ? bucket_index : runend_index;
}


inline int32_t FingerprintTable::offset_lower_bound(uint64_t slot_index) const {
    const Block *b = get_block(slot_index / slots_per_block_);
    const uint64_t slot_offset = slot_index % slots_per_block_;
    const uint64_t boffset = b->offset;
    const uint64_t occupieds = b->occupieds & fpt::bitmask(slot_offset + 1);
    if (boffset <= slot_offset) {
        const uint64_t runends = (b->runends & fpt::bitmask(slot_offset)) >> boffset;
        return __builtin_popcountll(occupieds) - __builtin_popcountll(runends);
    }
    return boffset - slot_offset + __builtin_popcountll(occupieds);
}


inline uint64_t FingerprintTable::find_first_empty_slot(uint64_t from) const {
    while (true) {
        const int32_t t = offset_lower_bound(from);
        assert(t >= 0);
        if (t == 0)
            break;
        from += t;
        if (from >= xnslots_)
            break;
    }
    return from;
}


/******************************************************************
 * Shifting slots and metadata around.                            *
 ******************************************************************/

inline void FingerprintTable::shift_remainders(uint64_t start_index, uint64_t empty_index) {
    uint64_t last_word = (empty_index + 1) * bits_per_slot_ / 64;
    const uint64_t first_word = start_index * bits_per_slot_ / 64;
    int bend = ((empty_index + 1) * bits_per_slot_) % 64;
    const int bstart = (start_index * bits_per_slot_) % 64;

    assert(first_word <= last_word);
    while (last_word != first_word) {
        *remainder_word(last_word) = fpt::shift_into_b(*remainder_word(last_word - 1),
                                                       *remainder_word(last_word),
                                                       0, bend, bits_per_slot_);
        last_word--;
        bend = 64;
    }
    *remainder_word(last_word) = fpt::shift_into_b(0, *remainder_word(last_word),
                                                   bstart, bend, bits_per_slot_);
}


inline void FingerprintTable::shift_slots(int64_t first, uint64_t last, uint64_t distance) {
    if (distance == 0)
        return;
    if (distance == 1) {
        shift_remainders(first, last + 1);
        return;
    }
    for (int64_t i = last; i >= first; i--)
        set_slot(i + distance, get_slot(i));
}


inline void FingerprintTable::shift_runends(int64_t first, uint64_t last, uint64_t distance) {
    assert(last < xnslots_ && distance < 64);
    uint64_t first_word = first / 64;
    uint64_t bstart = first % 64;
    uint64_t last_word = (last + distance + 1) / 64;
    uint64_t bend = (last + distance + 1) % 64;

    if (last_word != first_word) {
        const uint64_t first_runends_replacement = get_runends_word(first) & (~fpt::bitmask(bstart));
        do {
            set_runends_word(64 * last_word,
                             fpt::shift_into_b(last_word == first_word + 1
                                                  ? first_runends_replacement
                                                  : get_runends_word(64 * (last_word - 1)),
                                               get_runends_word(64 * last_word),
                                               0, bend, distance));
            bend = 64;
            last_word--;
        } while (last_word != first_word);
    }
    set_runends_word(64 * last_word, fpt::shift_into_b(0ULL, get_runends_word(64 * last_word),
                                                       bstart, bend, distance));
}


inline void FingerprintTable::remove_slot(bool only_item_in_run, uint64_t bucket_index,
                                          uint64_t remove_index) {
    // If we are removing the run's last slot, its predecessor becomes the new
    // run end.
    const bool was_runend = is_runend(remove_index);
    if (was_runend && !only_item_in_run)
        set_runend(remove_index - 1);

    // Walk to the end of the cluster the slot belongs to, then slide
    // everything after the hole back by one.
    uint64_t current_bucket = bucket_index;
    uint64_t current_slot = remove_index;
    if (!was_runend)
        while (!is_runend(current_slot))
            current_slot++;
    do {
        current_bucket++;
    } while (current_bucket <= current_slot && !is_occupied(current_bucket));
    while (current_bucket <= current_slot) {
        current_slot++;
        while (!is_runend(current_slot))
            current_slot++;
        do {
            current_bucket++;
        } while (current_bucket <= current_slot && !is_occupied(current_bucket));
    }
    const uint64_t last_slot_in_cluster = current_slot;

    uint64_t i;
    for (i = remove_index; i < last_slot_in_cluster; i++) {
        set_slot(i, get_slot(i + 1));
        if (is_runend(i) != is_runend(i + 1))
            flip_runend(i);
    }
    set_slot(i, 0);
    clear_runend(i);
    if (counters_ != nullptr)
        counters_->ShiftLeftAndClear(remove_index, last_slot_in_cluster);

    if (only_item_in_run)
        clear_occupied(bucket_index);

    // Repair the offsets of every block the shifted cluster spans.
    uint64_t block = bucket_index / slots_per_block_;
    while (block < last_slot_in_cluster / slots_per_block_) {
        const uint64_t last_occupieds_hash_index = slots_per_block_ * block + (slots_per_block_ - 1);
        const uint64_t runend_index = run_end(last_occupieds_hash_index);
        if (runend_index / slots_per_block_ == block) {
            get_block(block + 1)->offset = 0;
        }
        else {
            const uint32_t max_offset = (uint32_t) fpt::bitmask(8 * sizeof(Block::offset));
            const uint32_t new_offset = runend_index - last_occupieds_hash_index;
            get_block(block + 1)->offset = new_offset < max_offset ? new_offset : max_offset;
        }
        block++;
    }

    noccupied_slots_--;
}


/******************************************************************
 * Scanning a run.                                                *
 ******************************************************************/

inline int64_t FingerprintTable::longest_match_in_run(uint64_t runstart, uint64_t fingerprint) const {
    int64_t best = -1;
    uint64_t pos = runstart;
    while (true) {
        const uint64_t current = get_slot(pos);
        // Runs are sorted, and every fingerprint matching a full-length target
        // is numerically at most that target, so we are done once we pass it.
        if (current > fingerprint)
            break;
        // Matches of greater length are numerically larger, so the last match
        // in scan order is the longest one.
        if (fingerprints_match(current, fingerprint))
            best = pos;
        if (is_runend(pos))
            break;
        pos++;
    }
    return best;
}


inline uint64_t FingerprintTable::upper_bound_in_run(uint64_t runstart, uint64_t fingerprint) const {
    uint64_t pos = runstart;
    while (true) {
        if (get_slot(pos) > fingerprint)
            return pos;
        if (is_runend(pos))
            return pos + 1;
        pos++;
    }
}


/******************************************************************
 * Insertion.                                                     *
 ******************************************************************/

inline int64_t FingerprintTable::insert_fingerprint(uint64_t bucket_index, uint64_t fingerprint) {
    const uint64_t empty_slot = find_first_empty_slot(bucket_index);
    if (empty_slot >= xnslots_)
        return err_no_space;

    const uint64_t runend_index = run_end(bucket_index);
    uint64_t insert_index;
    if (is_occupied(bucket_index)) {
        insert_index = upper_bound_in_run(run_start(bucket_index), fingerprint);
        if (insert_index < empty_slot) {
            shift_slots(insert_index, empty_slot - 1, 1);
            shift_runends(insert_index, empty_slot - 1, 1);
        }
        // The run grew by one slot, so its end moves along with it.
        clear_runend(runend_index);
        set_runend(runend_index + 1);
    }
    else {
        if (bucket_index == empty_slot) {
            insert_index = bucket_index;
        }
        else {
            insert_index = runend_index + 1;
            if (insert_index < empty_slot) {
                shift_slots(insert_index, empty_slot - 1, 1);
                shift_runends(insert_index, empty_slot - 1, 1);
            }
        }
        set_runend(insert_index);
        set_occupied(bucket_index);
    }

    // Every block between the bucket and the slot we consumed now holds one
    // more slot's worth of spillover.
    for (uint64_t i = bucket_index / slots_per_block_ + 1;
            i <= empty_slot / slots_per_block_; i++) {
        if (get_block(i)->offset + 1ULL <= fpt::bitmask(8 * sizeof(Block::offset)))
            get_block(i)->offset++;
    }

    set_slot(insert_index, fingerprint);
    // The counters follow their slots, and the one the new fingerprint lands
    // on starts from zero.
    if (counters_ != nullptr)
        counters_->ShiftRightAndClear(insert_index, empty_slot);
    noccupied_slots_++;
    return insert_index;
}


/******************************************************************
 * The public interface.                                          *
 ******************************************************************/

inline uint64_t FingerprintTable::hash_key(uint64_t key, uint8_t flags) const {
    if ((flags & flag_key_is_hash) == 0) {
        if (hash_mode_ == hashmode::Default)
            key = MurmurHash64A(&key, sizeof(key), seed_);
        else if (hash_mode_ == hashmode::Invertible)
            key = hash_64(key, fpt::bitmask(63));
    }
    // The lowest `original_quotient_bits_` bits are fast-reduced into the
    // table's original slot count, which is what lets the table start at a
    // size that is not a power of two.
    const uint32_t oqs = original_quotient_bits_;
    const uint64_t fast_reduced_part = fast_reduce((key & fpt::bitmask(oqs)) << (32 - oqs),
                                                   original_nslots_);
    key &= ~fpt::bitmask(oqs);
    key |= fast_reduced_part;
    return key;
}


inline int32_t FingerprintTable::Insert(uint64_t key, uint8_t flags) {
    const int64_t ret = InsertAt(key, flags);
    return ret < 0 ? static_cast<int32_t>(ret) : 0;
}


inline int64_t FingerprintTable::InsertAt(uint64_t key, uint8_t flags) {
    if (auto_expand_ && noccupied_slots_ + 1 >= nslots_ * max_load_factor) {
        const int64_t ret = Expand();
        if (ret < 0)
            return ret;
    }

    const uint64_t hash = hash_key(key, flags);
    return insert_fingerprint(bucket_from_hash(hash), fingerprint_from_hash(hash, fingerprint_bits_));
}


inline void FingerprintTable::DeleteSlot(uint64_t bucket, uint64_t slot) {
    assert(is_occupied(bucket) && slot >= bucket && slot < xnslots_);
    const uint64_t runstart = run_start(bucket);
    const bool only_item_in_run = (slot == runstart) && is_runend(slot);
    remove_slot(only_item_in_run, bucket, slot);
}


inline int32_t FingerprintTable::Delete(uint64_t key, uint8_t flags) {
    const uint64_t hash = hash_key(key, flags);
    const uint64_t bucket_index = bucket_from_hash(hash);
    if (!is_occupied(bucket_index))
        return err_doesnt_exist;

    const uint64_t runstart = run_start(bucket_index);
    const uint64_t fingerprint = fingerprint_from_hash(hash, fingerprint_bits_);
    const int64_t pos = longest_match_in_run(runstart, fingerprint);
    if (pos < 0)
        return err_doesnt_exist;

    const bool only_item_in_run = (static_cast<uint64_t>(pos) == runstart) && is_runend(pos);
    remove_slot(only_item_in_run, bucket_index, pos);
    return 0;
}


inline uint64_t FingerprintTable::Count(uint64_t key, uint8_t flags) const {
    const uint64_t hash = hash_key(key, flags);
    const uint64_t bucket_index = bucket_from_hash(hash);
    if (!is_occupied(bucket_index))
        return 0;

    const uint64_t fingerprint = fingerprint_from_hash(hash, fingerprint_bits_);
    uint64_t res = 0;
    uint64_t pos = run_start(bucket_index);
    while (true) {
        const uint64_t current = get_slot(pos);
        if (current > fingerprint)
            break;
        res += fingerprints_match(current, fingerprint);
        if (is_runend(pos))
            break;
        pos++;
    }
    return res;
}


inline int64_t FingerprintTable::FindLongestMatch(uint64_t key, uint8_t flags) const {
    const uint64_t hash = hash_key(key, flags);
    const uint64_t bucket_index = bucket_from_hash(hash);
    // Issued here, before a single one of the table's own blocks is touched,
    // so that the counter chunk and the block travel from memory together. Put
    // it any later -- after `run_start`, say, which is what actually pins the
    // counter's index down -- and the two misses simply queue up behind one
    // another, which measures no better than not prefetching at all. The run
    // starts at or just past its bucket, so the bucket's chunk is nearly
    // always the one the count turns out to live in.
    if (counters_ != nullptr)
        counters_->Prefetch(bucket_index);
    if (!is_occupied(bucket_index))
        return -1;

    return longest_match_in_run(run_start(bucket_index),
                                fingerprint_from_hash(hash, fingerprint_bits_));
}


inline int32_t FingerprintTable::MatchLength(uint64_t key, uint8_t flags) const {
    const uint64_t hash = hash_key(key, flags);
    const uint64_t bucket_index = bucket_from_hash(hash);
    if (!is_occupied(bucket_index))
        return -1;

    const uint64_t fingerprint = fingerprint_from_hash(hash, fingerprint_bits_);
    const int64_t pos = longest_match_in_run(run_start(bucket_index), fingerprint);
    if (pos < 0)
        return -1;
    return static_cast<int32_t>(fpt::highbit_position(get_slot(pos)));
}


inline FingerprintTable::MatchSum
FingerprintTable::SumMatchingCounters(uint64_t key, uint8_t flags) const {
    MatchSum res;
    if (counters_ == nullptr)
        return res;

    const uint64_t hash = hash_key(key, flags);
    const uint64_t bucket_index = bucket_from_hash(hash);
    // Same reasoning as in `FindLongestMatch`: the counter chunk goes out
    // before any of the table's own blocks are touched, so that the two misses
    // travel together instead of queueing up behind one another.
    counters_->Prefetch(bucket_index);
    if (!is_occupied(bucket_index))
        return res;

    const uint64_t fingerprint = fingerprint_from_hash(hash, fingerprint_bits_);
    uint64_t pos = run_start(bucket_index);
    while (true) {
        const uint64_t current = get_slot(pos);
        // Runs are sorted, and every fingerprint matching a full-length target
        // is numerically at most that target, so we are done once we pass it.
        if (current > fingerprint)
            break;
        if (fingerprints_match(current, fingerprint)) {
            res.count++;
            res.sum += counters_->Get(pos);
        }
        if (is_runend(pos))
            break;
        pos++;
    }
    return res;
}


inline void FingerprintTable::collect_run(uint64_t bucket, std::vector<ChainedEntry>& out) const {
    out.clear();
    if (!is_occupied(bucket))
        return;
    uint64_t pos = run_start(bucket);
    while (true) {
        out.push_back({pos, bucket, get_slot(pos),
                       counters_ != nullptr ? counters_->Get(pos) : 0, 0});
        if (is_runend(pos))
            break;
        pos++;
    }
}


template <typename F>
inline void FingerprintTable::ForEachEntryWithChainSum(F&& f) const {
    assert(counters_ != nullptr);
    std::vector<ChainedEntry> run;

    // An entry's chain is the entries before it in the run that match it, so
    // a run has to be complete before any of its sums are.
    const auto sweep_run = [&]() {
        for (size_t j = 0; j < run.size(); j++) {
            uint64_t sum = run[j].count;
            for (size_t i = 0; i < j; i++)
                if (fingerprints_match(run[i].fingerprint, run[j].fingerprint))
                    sum += run[i].count;
            run[j].chain_sum = sum;
            f(const_cast<const ChainedEntry&>(run[j]));
        }
        run.clear();
    };

    for (auto it = begin(); it != end(); ++it) {
        if (!run.empty() && it.bucket() != run.front().bucket)
            sweep_run();
        run.push_back({it.slot(), it.bucket(), it.fingerprint(),
                       counters_->Get(it.slot()), 0});
    }
    sweep_run();
}


inline int64_t FingerprintTable::DeleteEntriesPreservingChainSums(
        std::vector<std::pair<uint64_t, uint64_t>>& victims) {
    if (victims.empty())
        return 0;
    assert(counters_ != nullptr);

    // Highest slot first. Removing an entry slides the rest of its cluster
    // down over the hole, so the slots above it move and the ones below do
    // not: going downwards, every victim is still where it was found. A run
    // occupies a contiguous stretch of slots, so this also groups the victims
    // of each run together.
    std::sort(victims.begin(), victims.end(),
              [](const std::pair<uint64_t, uint64_t>& a,
                 const std::pair<uint64_t, uint64_t>& b) { return a.second > b.second; });

    // What each run has to be repaired by, worked out before anything moves.
    struct Repair {
        uint64_t bucket;
        std::vector<uint64_t> additions;    /**< One per surviving entry, in run order. */
    };
    std::vector<Repair> repairs;
    std::vector<ChainedEntry> run;
    std::vector<bool> doomed;
    int64_t delta = 0;

    for (size_t at = 0; at < victims.size();) {
        const uint64_t bucket = victims[at].first;
        size_t group_end = at;
        while (group_end < victims.size() && victims[group_end].first == bucket)
            group_end++;

        collect_run(bucket, run);
        doomed.assign(run.size(), false);
        for (size_t i = at; i < group_end; i++) {
            const auto entry = std::find_if(run.begin(), run.end(),
                    [&](const ChainedEntry& e) { return e.slot == victims[i].second; });
            assert(entry != run.end());
            doomed[entry - run.begin()] = true;
            delta -= static_cast<int64_t>(entry->count);
        }

        Repair repair{bucket, {}};
        for (size_t j = 0; j < run.size(); j++) {
            if (doomed[j])
                continue;
            // Whatever was removed from under this entry, and whether anything
            // is left between it and the bottom of its chain.
            uint64_t removed = 0;
            bool has_surviving_prefix = false;
            for (size_t i = 0; i < j; i++) {
                if (!fingerprints_match(run[i].fingerprint, run[j].fingerprint))
                    continue;
                if (doomed[i])
                    removed += run[i].count;
                else
                    has_surviving_prefix = true;
            }
            // Only the entries that lose their last surviving prefix take the
            // removed counters on: every entry above one of those reaches the
            // same removed entries through it, so its chain sum is repaired
            // along with theirs.
            const uint64_t addition = has_surviving_prefix ? 0 : removed;
            repair.additions.push_back(addition);
            delta += static_cast<int64_t>(addition);
        }
        if (!repair.additions.empty())
            repairs.push_back(std::move(repair));
        at = group_end;
    }

    for (const auto& [bucket, slot] : victims)
        DeleteSlot(bucket, slot);

    // The survivors kept their order within the run, so the repairs line up
    // with a plain walk of what is left of it.
    for (const Repair& repair : repairs) {
        assert(is_occupied(repair.bucket));
        uint64_t pos = run_start(repair.bucket);
        for (const uint64_t addition : repair.additions) {
            if (addition != 0)
                counters_->Set(pos, counters_->Get(pos) + addition);
            pos++;
        }
    }
    return delta;
}


inline int32_t FingerprintTable::DeletePreservingChainSums(uint64_t key, uint8_t flags,
                                                           int64_t& counter_delta) {
    const uint64_t hash = hash_key(key, flags);
    const uint64_t bucket_index = bucket_from_hash(hash);
    counter_delta = 0;
    if (!is_occupied(bucket_index))
        return err_doesnt_exist;

    const int64_t pos = longest_match_in_run(run_start(bucket_index),
                                             fingerprint_from_hash(hash, fingerprint_bits_));
    if (pos < 0)
        return err_doesnt_exist;

    std::vector<std::pair<uint64_t, uint64_t>> victim{{bucket_index,
                                                       static_cast<uint64_t>(pos)}};
    counter_delta = DeleteEntriesPreservingChainSums(victim);
    return 0;
}


/******************************************************************
 * Expansion and contraction.                                     *
 ******************************************************************/

inline int64_t FingerprintTable::rebuild_into(FingerprintTable& dest, rebuild_op op) const {
    const uint32_t dest_bihs = dest.GetBucketIndexHashSize();
    int64_t moved = 0;
    for (auto it = begin(); it != end(); ++it) {
        const uint64_t hash = it.hash();
        const uint32_t len = it.fingerprint_length();
        // A count belongs to its fingerprint, so it travels with it.
        const uint64_t count = (counters_ != nullptr ? counters_->Get(it.slot()) : 0);

        int64_t at = 0;
        switch (op) {
            case rebuild_op::restretch:
                // A Stretching step, in either direction. The destination
                // reads the same bits of the hash as we do and derives the
                // same base bucket from them; only the slot that bucket is
                // stretched to differs, and `dest.insert_hash_at` works that
                // out on its own. The fingerprint is untouched.
                at = dest.insert_hash_at(hash, len);
                moved++;
                break;

            case rebuild_op::sacrifice:
                if (len == 0) {
                    // A void entry: its fingerprint has no bit left to donate
                    // to the bucket index, so the item may belong to either of
                    // the two candidate buckets. Store it in both, as Aleph
                    // filter does, which keeps queries free of false negatives.
                    const uint64_t hash_1 = hash & fpt::bitmask(dest_bihs - 1);
                    const uint64_t hash_2 = hash_1 | (1ULL << (dest_bihs - 1));
                    at = dest.insert_hash_at(hash_1, 0);
                    if (at >= 0) {
                        // Both copies carry the count. Either one of them may
                        // be the entry a later query lands on, and neither has
                        // a fingerprint left to tell it apart from the other,
                        // so the only answer that stays an over-estimate --
                        // never an under-estimate -- is to give each the whole
                        // count. Setting this one before inserting the second
                        // is safe: that insertion shifts slots, and the
                        // counters shift right along with them.
                        dest.set_count(at, count);
                        at = dest.insert_hash_at(hash_2, 0);
                    }
                    moved += 2;
                }
                else {
                    at = dest.insert_hash_at(hash, len - 1);
                    moved++;
                }
                break;

            case rebuild_op::unsacrifice:
                // Hands a bit of the bucket index back to the fingerprint, up
                // to the width of a slot. Capping at `fingerprint_bits_`
                // yields exactly the fingerprint a fresh insertion into the
                // smaller table would produce. The duplicated copies of a void
                // entry are not merged back into one, so each keeps its count.
                at = dest.insert_hash_at(hash, std::min<uint64_t>(len + 1, dest.fingerprint_bits_));
                moved++;
                break;
        }

        if (at < 0)
            return at;
        dest.set_count(at, count);
    }
    return moved;
}


inline int64_t FingerprintTable::Expand() {
    // The last epoch of a period is the one that doubles the base address
    // space and takes a bit off every fingerprint; the `r - 1` before it just
    // stretch the same buckets over more slots. With `r == 1` every expansion
    // is the last one, which is plain doubling.
    const bool ending_period = (epoch_ + 1 >= growth_coefficient_);
    if (ending_period && key_bits_ + 2 > 64)
        return err_no_space;

    const Shape shape{original_nslots_,
                      ending_period ? base_nslots_ * 2 : base_nslots_,
                      key_bits_ + ending_period,
                      original_quotient_bits_,
                      ending_period ? 0u : epoch_ + 1};
    FingerprintTable expanded(shape, hash_mode_, seed_, growth_coefficient_);
    expanded.auto_expand_ = auto_expand_;
    expanded.mirror_counters_of(*this);
    const int64_t res = rebuild_into(expanded, ending_period ? rebuild_op::sacrifice
                                                             : rebuild_op::restretch);
    if (res < 0)
        return res;

    *this = std::move(expanded);
    return res;
}


inline int64_t FingerprintTable::Contract() {
    if (GetExpansionCount() == 0)
        return err_cannot_contract;

    // Undo whatever the matching expansion did: step back an epoch, or, if we
    // are sitting at the start of a period, reopen the previous one at its
    // last epoch and give every fingerprint its bit back.
    const bool reopening_period = (epoch_ == 0);
    assert(!reopening_period || base_nslots_ % 2 == 0);
    const Shape shape{original_nslots_,
                      reopening_period ? base_nslots_ / 2 : base_nslots_,
                      key_bits_ - reopening_period,
                      original_quotient_bits_,
                      reopening_period ? growth_coefficient_ - 1 : epoch_ - 1};
    FingerprintTable contracted(shape, hash_mode_, seed_, growth_coefficient_);
    contracted.auto_expand_ = auto_expand_;
    contracted.mirror_counters_of(*this);
    const int64_t res = rebuild_into(contracted, reopening_period ? rebuild_op::unsacrifice
                                                                  : rebuild_op::restretch);
    if (res < 0)
        return res;

    *this = std::move(contracted);
    return res;
}


/******************************************************************
 * Iteration.                                                     *
 ******************************************************************/

inline FingerprintTable::const_iterator FingerprintTable::begin() const {
    const_iterator it(*this);
    if (noccupied_slots_ == 0)
        return end();

    // Find the first run.
    uint64_t block_index = 0;
    uint64_t idx = fpt::lowbit_position(get_block(0)->occupieds);
    while (idx == 64) {
        block_index++;
        if (block_index >= nblocks_)
            return end();
        idx = fpt::lowbit_position(get_block(block_index)->occupieds);
    }
    const uint64_t position = block_index * slots_per_block_ + idx;

    it.run_ = position;
    it.current_ = std::max(run_start(position), position);
    if (it.current_ >= xnslots_)
        return end();
    return it;
}


inline FingerprintTable::const_iterator FingerprintTable::end() const {
    const_iterator it(*this);
    it.run_ = const_iterator::end_marker_;
    it.current_ = const_iterator::end_marker_;
    return it;
}


inline FingerprintTable::const_iterator& FingerprintTable::const_iterator::operator++() {
    if (current_ == end_marker_)
        return *this;

    if (!table_->is_runend(current_)) {
        current_++;
        return *this;
    }

    // Move on to the next run.
    uint64_t block_index = run_ / slots_per_block_;
    uint64_t rank = fpt::bitrank_inclusive(table_->get_block(block_index)->occupieds,
                                           run_ % slots_per_block_);
    uint64_t next_run = bit_select(table_->get_block(block_index)->occupieds, rank);
    while (next_run == 64) {
        block_index++;
        if (block_index >= table_->nblocks_) {
            run_ = current_ = end_marker_;
            return *this;
        }
        next_run = bit_select(table_->get_block(block_index)->occupieds, 0);
    }

    run_ = block_index * slots_per_block_ + next_run;
    current_++;
    if (current_ < run_)
        current_ = run_;
    if (current_ >= table_->xnslots_)
        run_ = current_ = end_marker_;
    return *this;
}


inline uint64_t FingerprintTable::const_iterator::hash() const {
    const uint32_t bihs = table_->GetBucketIndexHashSize();
    const uint32_t oqs = table_->original_quotient_bits_;
    // `run_` is a canonical slot, so it is in the image of `stretch` and the
    // base bucket it came from can be recovered exactly.
    const uint64_t bucket = table_->unstretch(run_);
    const uint64_t original_bucket_index = bucket >> (bihs - oqs);
    const uint64_t bucket_extension = (bucket & fpt::bitmask(bihs - oqs)) << oqs;
    return original_bucket_index | (fingerprint() << bihs) | bucket_extension;
}


/******************************************************************
 * Debugging.                                                     *
 ******************************************************************/

inline void FingerprintTable::DebugDumpBlock(uint64_t i) const {
    std::cerr << "============================= block " << i
              << " offset=" << +get_block(i)->offset << std::endl;
    for (uint32_t j = 0; j < slots_per_block_; j++) {
        const uint64_t ind = slots_per_block_ * i + j;
        if (ind >= xnslots_)
            break;
        const uint64_t slot = get_slot(ind);
        std::cerr << '@' << ind << '(' << j << "):" << is_occupied(ind) << ',' << is_runend(ind) << ',';
        for (int32_t k = bits_per_slot_ - 1; k >= 0; k--)
            std::cerr << ((slot >> k) & 1);
        std::cerr << ' ';
    }
    std::cerr << std::endl << std::endl;
}


inline void FingerprintTable::DebugDumpMetadata() const {
    std::cerr << "Slots: " << nslots_
              << " Blocks: " << nblocks_
              << " Occupied: " << noccupied_slots_ << std::endl;
    std::cerr << "Key bits: " << key_bits_
              << " Fingerprint bits: " << fingerprint_bits_
              << " Original quotient bits: " << original_quotient_bits_
              << " Expansions: " << GetExpansionCount()
              << " --- Bits per slot: " << bits_per_slot_ << std::endl;
    std::cerr << "Growth coefficient: " << growth_coefficient_
              << " Epoch: " << epoch_
              << " Periods: " << GetPeriodCount()
              << " Base slots: " << base_nslots_
              << " Original slots: " << original_nslots_ << std::endl;
}

}   // namespace sublime
