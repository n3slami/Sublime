#pragma once

/*
 * ============================================================================
 *
 *        FingerprintTable
 *          A compact hash table storing a multiset of fixed-length
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
 * hashing semantics and Zeno filter's Stretching, but with all of Memento
 * filter's memento/keepsake-box machinery removed. Every stored item occupies exactly one slot holding just
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
 * `key_bits` is fixed for the life of the table. Expanding by a factor of two
 * increases BIHS by one, so the *least* significant bit of every fingerprint
 * is donated to the bucket index and `L` falls by one; contracting hands the
 * bit back. The hash the table keeps of a key -- its bucket together with its
 * fingerprint -- is therefore always exactly `key_bits` wide, however often
 * the table has been resized.
 *
 * ---------------------------------------------------------------------------
 * Fixed-length fingerprints
 * ---------------------------------------------------------------------------
 * A slot is exactly `L = fingerprint_bits` bits wide and holds
 *
 *      hash bits [BIHS, BIHS + L)
 *
 * and nothing else. Every fingerprint in the table is of that one length, the
 * length a fresh insertion would produce, because an expansion shortens the
 * stored ones and `fingerprint_bits` in the same step. So two fingerprints
 * match when they are *equal*, one key corresponds to exactly one entry, and a
 * run is simply held in ascending fingerprint order.
 *
 * Shortening is what a resize costs in accuracy: a shorter fingerprint stands
 * for a larger family of keys, so entries the table could once tell apart
 * merge into one. Because both the stored and the fresh length shrink
 * together, a slot also gets one bit narrower per doubling, which gives some
 * of that space back.
 *
 * Contraction is the exact inverse of expansion -- an entry's `key_bits`-wide
 * hash survives both, so a resize is a pure re-hash and round-trips exactly.
 * The table can only shed as many bits as it has: `Expand` refuses once the
 * fingerprint is down to a single bit.
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

    /**
     * Removes the entries sitting in the given `(bucket, slot)` pairs. The
     * pairs normally come from a pass that has just located them, such as an
     * eviction sweep.
     *
     * @param victims The entries to remove, in any order. Left reordered:
     * sorting them highest-slot-first is what makes the removals independent,
     * since removing a slot only moves the slots above it.
     */
    void DeleteSlots(std::vector<std::pair<uint64_t, uint64_t>>& victims);

    /**
     * @returns The canonical bucket of the run the entry in `slot` belongs to,
     * which is what `DeleteSlot` needs and a scan that found the slot by any
     * other route -- a counter, say -- does not have.
     *
     * Found by walking back to the start of the slot's cluster, which is
     * always the canonical slot of the run that opens it, and then pairing
     * runs with occupied buckets forward from there. That is the same walk
     * `remove_slot` makes, and costs the same: the length of the cluster.
     */
    uint64_t BucketOfSlot(uint64_t slot) const {
        assert(slot < xnslots_);
        assert(offset_lower_bound(slot) != 0);      // The slot has to be in use.
        uint64_t start = slot;
        while (start > 0 && offset_lower_bound(start - 1) != 0)
            start--;
        assert(is_occupied(start));

        uint64_t bucket = start, pos = start;
        while (true) {
            uint64_t end = pos;
            while (!is_runend(end))
                end++;
            if (slot <= end)
                return bucket;
            pos = end + 1;
            do {
                bucket++;
            } while (!is_occupied(bucket));
        }
    }

    /**
     * @returns The bits of `key`'s hash that the table keeps: its bucket and
     * its fingerprint. Two keys share this value exactly when no insertion or
     * query can tell them apart.
     */
    uint64_t EntryIdentity(uint64_t key, uint8_t flags = 0) const {
        return hash_key(key, flags) & fpt::bitmask(key_bits_);
    }

    /**
     * @param key The key to look up.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns The number of stored fingerprints equal to `key`'s. This never
     * undercounts how many times `key` was inserted, but it may overcount, as
     * a fingerprint of another key may match by chance.
     */
    uint64_t Count(uint64_t key, uint8_t flags = 0) const;

    /**
     * @param key The key to look up.
     * @param flags Set `flag_key_is_hash` if `key` is already hashed.
     * @returns `true` if any stored fingerprint matches `key`'s hash.
     */
    bool Contains(uint64_t key, uint8_t flags = 0) const {
        return FindMatch(key, flags) >= 0;
    }

    /**
     * @returns The slot holding the stored fingerprint matching `key`, or -1
     * if the key's run holds no match at all. That slot is where a
     * mirroring counter array keeps the fingerprint's count, and the search
     * starts that counter's chunk on its way into cache before it touches the
     * table at all, so that reading or updating the count does not stall on a
     * second miss queued up behind this one.
     */
    int64_t FindMatch(uint64_t key, uint8_t flags = 0) const;

    /**
     * Doubles the number of slots. Every stored fingerprint donates its lowest
     * bit to the bucket index, so the fingerprint length -- stored and fresh
     * alike -- falls by one and a slot gets one bit narrower.
     *
     * Counts, if the table is keeping any, follow their fingerprints across.
     *
     * @returns The number of fingerprints in the expanded table, or a negative
     * status code on failure, in particular `err_no_space` if the fingerprint
     * is down to its last bit. The table is left untouched on failure.
     */
    int64_t Expand();

    /**
     * Halves the number of slots, the exact inverse of `Expand`: the bucket
     * index gives a bit back to every fingerprint, which therefore grows by
     * one bit.
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
    void EnableCounters(bool with_min_tree = false) {
        counters_ = std::make_unique<VALECounters>(xnslots_, with_min_tree);
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

    /* Slot sidecar. */

    /**
     * A caller-owned per-slot array the table keeps aligned to its slots,
     * mirroring the slot movements of insertion and deletion the way it does
     * for its own counters. Unlike `EnableCounters` -- which the table owns and
     * reshapes -- a sidecar holds whatever per-slot data the caller likes (a
     * plain counter array, a packed heap-position array, ...) and only the
     * shift-on-insert, shift-on-delete, and reset movements are mirrored;
     * expansion and contraction are not, so a table carrying a sidecar must not
     * be expanded or contracted.
     */
    struct SlotMirror {
        virtual ~SlotMirror() = default;
        /** Fills the hole at `hole` by moving `(hole, last]` down one, clearing `last`. */
        virtual void ShiftLeftAndClear(uint64_t hole, uint64_t last) = 0;
        /** Opens a hole at `hole` by moving `[hole, last)` up one, clearing `hole`. */
        virtual void ShiftRightAndClear(uint64_t hole, uint64_t last) = 0;
        /**
         * Moves one slot's data to another, and clears one outright. This
         * table never does either -- it only ever shifts -- but `CuckooTable`
         * only ever does these, and declaring both here is what lets one
         * sidecar serve whichever table its owner was built on.
         */
        virtual void MoveSlot(uint64_t from, uint64_t to) = 0;
        virtual void SwapWithHeld(uint64_t slot) = 0;
        /** Empties that hand, before a kick path starts carrying. */
        virtual void ClearHeld() = 0;
        virtual void Clear(uint64_t slot) = 0;
        /** Clears every entry. */
        virtual void Reset() = 0;
    };

    /** Attaches a caller-owned sidecar (or detaches with `nullptr`). */
    void AttachMirror(SlotMirror *mirror) {
        sidecar_ = mirror;
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
        if (ending_period && fingerprint_bits_ <= 1)
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
        /**
         * @returns The `key_bits`-wide hash of the item pointed at, laid out
         * the way `Insert` expects a pre-hashed key. The bucket and the
         * fingerprint together carry every bit of it, so nothing is lost and a
         * resize can re-split it however its own shape dictates.
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
    uint64_t fingerprint_bits_;         /**< The fingerprint length, which every stored fingerprint shares. */
    uint64_t bits_per_slot_;            /**< The same thing: a slot holds a fingerprint and nothing else. */
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
    /** An optional caller-owned per-slot array, mirrored on shifts. See `SlotMirror`. */
    SlotMirror *sidecar_ = nullptr;
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

    /** @returns The slot contents holding `hash`'s fingerprint. */
    uint64_t fingerprint_from_hash(uint64_t hash) const {
        return (hash >> GetBucketIndexHashSize()) & fpt::bitmask(fingerprint_bits_);
    }

    /**
     * @param runstart The first slot of the run to scan.
     * @param fingerprint The fingerprint to look for.
     * @returns The position of the fingerprint in the run equal to
     * `fingerprint`, or -1 if there is none.
     */
    int64_t find_in_run(uint64_t runstart, uint64_t fingerprint) const;

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

    /** Inserts the fingerprint of the pre-folded `hash`. */
    int32_t insert_hash(uint64_t hash) {
        const int64_t ret = insert_hash_at(hash);
        return ret < 0 ? static_cast<int32_t>(ret) : 0;
    }

    /** As `insert_hash`, but reporting the slot the fingerprint landed in. */
    int64_t insert_hash_at(uint64_t hash) {
        return insert_fingerprint(bucket_from_hash(hash), fingerprint_from_hash(hash));
    }

    /**
     * Re-inserts every entry of this table into `dest`, which differs from it
     * in size. Each entry carries its whole `key_bits`-wide hash over, and
     * `dest` splits it into a bucket and a fingerprint by its own shape, so
     * the same routine serves a stretch, an expansion and a contraction alike.
     */
    int64_t rebuild_into(FingerprintTable& dest) const;
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

    // Every fingerprint is of the same length, so a slot holds one and
    // nothing else -- and gets one bit narrower with every doubling.
    bits_per_slot_ = fingerprint_bits_;
    original_quotient_bits_ = shape.orig_quotient_bits ? shape.orig_quotient_bits
                                                       : key_bits_ - fingerprint_bits_;

    // A slot must be addressable by the 64-bit reads in `get_slot`, and it
    // has to be wide enough to tell anything apart at all.
    assert(bits_per_slot_ <= 56);
    assert(fingerprint_bits_ >= 1);
    assert(key_bits_ <= 64);
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
    if (sidecar_ != nullptr)
        sidecar_->Reset();
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
    if (sidecar_ != nullptr)
        sidecar_->ShiftLeftAndClear(remove_index, last_slot_in_cluster);

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

inline int64_t FingerprintTable::find_in_run(uint64_t runstart, uint64_t fingerprint) const {
    uint64_t pos = runstart;
    while (true) {
        const uint64_t current = get_slot(pos);
        // Runs are held in ascending fingerprint order, so we are done once we
        // pass the one we are looking for.
        if (current > fingerprint)
            break;
        if (current == fingerprint)
            return static_cast<int64_t>(pos);
        if (is_runend(pos))
            break;
        pos++;
    }
    return -1;
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
    if (sidecar_ != nullptr)
        sidecar_->ShiftRightAndClear(insert_index, empty_slot);
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
    return insert_fingerprint(bucket_from_hash(hash), fingerprint_from_hash(hash));
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
    const uint64_t fingerprint = fingerprint_from_hash(hash);
    const int64_t pos = find_in_run(runstart, fingerprint);
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

    const uint64_t fingerprint = fingerprint_from_hash(hash);
    uint64_t res = 0;
    uint64_t pos = run_start(bucket_index);
    while (true) {
        const uint64_t current = get_slot(pos);
        if (current > fingerprint)
            break;
        res += (current == fingerprint);
        if (is_runend(pos))
            break;
        pos++;
    }
    return res;
}


inline int64_t FingerprintTable::FindMatch(uint64_t key, uint8_t flags) const {
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
    // Prefetching the table's *own* memory here -- the slots of the bucket's
    // block, a cache line or two past the metadata that locates them -- was
    // tried and does nothing (within 2% either way at 64k and 256k slots).
    // There is no second miss to overlap with: the metadata has to arrive
    // before the slot index even exists, so the two are serial whatever is
    // issued when. The counter prefetch above is different only because the
    // counter's address is known from the bucket alone. `CuckooTable` is the
    // structure that can win here, having two independent buckets to read.
    if (!is_occupied(bucket_index))
        return -1;

    return find_in_run(run_start(bucket_index), fingerprint_from_hash(hash));
}


inline void FingerprintTable::DeleteSlots(std::vector<std::pair<uint64_t, uint64_t>>& victims) {
    if (victims.empty())
        return;

    // Highest slot first. Removing an entry slides the rest of its cluster
    // down over the hole, so the slots above it move and the ones below do
    // not: going downwards, every victim is still where it was found.
    std::sort(victims.begin(), victims.end(),
              [](const std::pair<uint64_t, uint64_t>& a,
                 const std::pair<uint64_t, uint64_t>& b) { return a.second > b.second; });
    for (const auto& [bucket, slot] : victims)
        DeleteSlot(bucket, slot);
}


/******************************************************************
 * Expansion and contraction.                                     *
 ******************************************************************/

inline int64_t FingerprintTable::rebuild_into(FingerprintTable& dest) const {
    int64_t moved = 0;
    for (auto it = begin(); it != end(); ++it) {
        // An entry's bucket and fingerprint carry its whole `key_bits`-wide
        // hash between them, and `key_bits` is the one thing a resize leaves
        // alone -- so handing `dest` the hash is all there is to it, whether
        // it is wider than us, narrower, or merely stretched differently. It
        // splits the hash by its own shape. A count belongs to its
        // fingerprint, so it travels with it.
        const uint64_t hash = it.hash();
        const uint64_t count = (counters_ != nullptr ? counters_->Get(it.slot()) : 0);

        const int64_t at = dest.insert_hash_at(hash);
        if (at < 0)
            return at;
        dest.set_count(at, count);
        moved++;
    }
    return moved;
}


inline int64_t FingerprintTable::Expand() {
    // The last epoch of a period is the one that doubles the base address
    // space and takes a bit off every fingerprint; the `r - 1` before it just
    // stretch the same buckets over more slots. With `r == 1` every expansion
    // is the last one, which is plain doubling.
    const bool ending_period = (epoch_ + 1 >= growth_coefficient_);
    // The doubling takes a bit off every fingerprint, and a fingerprint has to
    // keep at least one: a zero-width slot holds nothing to tell keys apart by.
    if (ending_period && fingerprint_bits_ <= 1)
        return err_no_space;

    const Shape shape{original_nslots_,
                      ending_period ? base_nslots_ * 2 : base_nslots_,
                      key_bits_,
                      original_quotient_bits_,
                      ending_period ? 0u : epoch_ + 1};
    FingerprintTable expanded(shape, hash_mode_, seed_, growth_coefficient_);
    expanded.auto_expand_ = auto_expand_;
    expanded.mirror_counters_of(*this);
    // Every counter is about to be written, so a min tree is better rebuilt
    // once at the end than climbed per entry.
    if (expanded.counters_ != nullptr)
        expanded.counters_->SuspendTree();
    const int64_t res = rebuild_into(expanded);
    if (expanded.counters_ != nullptr)
        expanded.counters_->RebuildTree();
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
    // last epoch and give every fingerprint its bit back. Either way it is an
    // exact inverse, since the hash a rebuild carries over is untouched.
    const bool reopening_period = (epoch_ == 0);
    assert(!reopening_period || base_nslots_ % 2 == 0);
    const Shape shape{original_nslots_,
                      reopening_period ? base_nslots_ / 2 : base_nslots_,
                      key_bits_,
                      original_quotient_bits_,
                      reopening_period ? growth_coefficient_ - 1 : epoch_ - 1};
    FingerprintTable contracted(shape, hash_mode_, seed_, growth_coefficient_);
    contracted.auto_expand_ = auto_expand_;
    contracted.mirror_counters_of(*this);
    if (contracted.counters_ != nullptr)
        contracted.counters_->SuspendTree();
    const int64_t res = rebuild_into(contracted);
    if (contracted.counters_ != nullptr)
        contracted.counters_->RebuildTree();
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
