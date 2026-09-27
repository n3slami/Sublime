#pragma once

/*
 * ============================================================================
 *
 *        MG
 *          A classic Misra-Gries summary, storing fingerprints.
 *
 * ============================================================================
 *
 * The textbook Misra-Gries: a bounded set of monitored keys, each with a count;
 * an already-monitored key's count goes up, a key that finds room is admitted
 * at one, and a key that finds the summary full costs every count one and
 * evicts whatever reaches zero. The decrement is a single sweep of the entries,
 * which is also what finds the ones to evict -- no auxiliary heap, no
 * bucket list, nothing but the counts themselves.
 *
 * The only twist is where the monitored set lives -- in a fixed-size RSQF
 * `FingerprintTable` storing one fingerprint per key, rather than a hash map of
 * full keys -- so its per-key footprint is comparable to `SublimeMG`'s. That is
 * what makes it the fair baseline: same monitored set, same algorithm, and the
 * difference in the numbers is Sublime's.
 *
 * The table never expands, and its counts are a plain `uint32_t` array rather
 * than VALE's variable-length counters. Both of those are deliberate: they are
 * exactly what `SublimeMG` adds.
 *
 * Two keys sharing a fingerprint are merged into one entry, which only ever
 * over-estimates -- the same trade `SublimeMG` makes.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "FingerprintTable.hpp"

namespace sublime {

/**
 * @tparam Table The monitored-key set: `FingerprintTable` or `CuckooTable`.
 * The baseline is built on whichever `SublimeMG` is being compared against.
 */
template <typename Table = FingerprintTable>
class MG {
    friend class MGTest;

public:
    /** The monitored-key set this was instantiated over. */
    using table_type = Table;
    using hashmode = typename Table::hashmode;

    /** Signals that the key passed in has already been hashed. */
    static constexpr uint32_t flag_key_is_hash = Table::flag_key_is_hash;
    static constexpr int32_t err_no_space = Table::err_no_space;

    /** The default length, in bits, of a stored fingerprint. */
    static constexpr uint32_t default_fingerprint_length = 32;

    /**
     * @param nslots The number of slots the fingerprint table holds. `Capacity`
     * is a shade under this.
     * @param hash_mode The hashing mode, see `FingerprintTable::hashmode`.
     * @param seed The seed of the hash function.
     * @param fingerprint_length The length, in bits, of each stored fingerprint.
     */
    MG(uint64_t nslots, hashmode hash_mode, uint32_t seed,
       uint32_t fingerprint_length = default_fingerprint_length):
            table_{nslots, key_bits_for(nslots, fingerprint_length), hash_mode, seed},
            sidecar_{table_.GetSlotCapacity()} {
        table_.AttachMirror(&sidecar_);
    }

    // The table stores a raw pointer to `sidecar_`, so the object must not move.
    MG(const MG&) = delete;
    MG& operator=(const MG&) = delete;
    MG(MG&&) = delete;
    MG& operator=(MG&&) = delete;

    /**
     * Counts one more occurrence of `key`, applying whichever Misra-Gries case
     * fits: an already-monitored key's count goes up, a key with room to spare
     * is admitted at one, and otherwise every count comes down by one, anything
     * that reaches zero is evicted, and this occurrence takes one of the slots
     * that frees up -- or is dropped, if nothing reached zero.
     *
     * @returns 0, or `err_no_space`.
     */
    int32_t Insert(uint64_t key, uint8_t flags = 0) {
        n_++;
        const int64_t slot = table_.FindMatch(key, flags);
        if (slot >= 0) {                            // Already monitored.
            sidecar_.counts[slot]++;
            return 0;
        }
        if (CountMonitored() < Capacity())          // Room to admit it.
            return admit(key, flags);

        // Full: the Misra-Gries decrement, and the occurrence that paid for it
        // takes one of the slots it emptied, if it emptied any.
        return decrement_pass() > 0 ? admit(key, flags) : 0;
    }

    /**
     * Estimates the frequency of `key`: the count stored on the single
     * fingerprint matching it, or 0 if nothing matches. A fingerprint collision
     * can only make this an over-estimate.
     */
    uint64_t Query(uint64_t key, uint8_t flags = 0) const {
        const int64_t slot = table_.FindMatch(key, flags);
        return slot < 0 ? 0 : sidecar_.counts[slot];
    }

    /** @returns True if `key` is monitored. */
    bool IsMonitored(uint64_t key, uint8_t flags = 0) const {
        return table_.FindMatch(key, flags) >= 0;
    }

    /** A no-op, for interface parity with the other sketches' prefetch queues. */
    void FlushPrefetchQueue() {}

    /** Empties the summary, keeping its shape. */
    void Reset() {
        table_.Reset();     // Resets the sidecar with it.
        total_decrements_ = 0;
        n_ = 0;
    }

    /* Size and shape. */

    /** @returns How many keys the summary can monitor at once. */
    uint64_t Capacity() const {
        return static_cast<uint64_t>(table_.CountSlots() * Table::max_load_factor);
    }
    /** @returns The number of keys currently monitored. */
    uint64_t CountMonitored() const {
        return table_.CountFingerprints();
    }
    /**
     * @returns How many times every count came down by one, i.e. how many
     * decrement sweeps there have been. This is the classic Misra-Gries
     * parameter: no key's count is understated by more than this.
     */
    uint64_t CountDecrements() const {
        return total_decrements_;
    }
    /** @returns `N`, the number of insertions the summary has seen. */
    uint64_t GetStreamLength() const {
        return n_;
    }
    /** @returns The bytes held by the fingerprints and the counts. */
    uint64_t SizeInBytes() const {
        return table_.SizeInBytes() + sidecar_.counts.capacity() * sizeof(uint32_t);
    }
    /** Alias used by the benchmark harness. */
    uint64_t Size() const {
        return SizeInBytes();
    }

    const Table& GetTable() const {
        return table_;
    }

private:
    /**
     * The per-slot counts, which the table keeps aligned to its fingerprints:
     * each one follows its fingerprint through every slot shift.
     */
    struct Sidecar : Table::SlotMirror {
        std::vector<uint32_t> counts;
        /** One entry's count, in hand while a cuckoo kick path carries it. */
        uint32_t held = 0;

        explicit Sidecar(uint64_t nslots_alloc): counts(nslots_alloc, 0) {}

        // Net effect of the RSQF closing a hole: [hole, last) take their right
        // neighbour, and `last` is cleared.
        void ShiftLeftAndClear(uint64_t hole, uint64_t last) override {
            for (uint64_t i = hole; i < last; i++)
                counts[i] = counts[i + 1];
            counts[last] = 0;
        }

        // Net effect of the RSQF opening a hole: (hole, last] take their left
        // neighbour, and `hole` is cleared.
        void ShiftRightAndClear(uint64_t hole, uint64_t last) override {
            for (uint64_t i = last; i > hole; i--)
                counts[i] = counts[i - 1];
            counts[hole] = 0;
        }

        // What the cuckoo table does instead of shifting: one entry moves.
        void MoveSlot(uint64_t from, uint64_t to) override {
            counts[to] = counts[from];
            counts[from] = 0;
        }

        void SwapWithHeld(uint64_t slot) override {
            std::swap(counts[slot], held);
        }

        void ClearHeld() override {
            held = 0;
        }

        void Clear(uint64_t slot) override {
            counts[slot] = 0;
        }

        void Reset() override {
            std::fill(counts.begin(), counts.end(), 0);
        }
    };

    Table table_;
    Sidecar sidecar_;
    uint64_t total_decrements_ = 0;
    uint64_t n_ = 0;

    /** The `key_bits` that make a freshly stored fingerprint `fp_len` bits long. */
    static uint64_t key_bits_for(uint64_t nslots, uint32_t fp_len) {
        uint64_t quotient_bits = 0;
        for (uint64_t n = nslots; n > 1; n >>= 1)
            quotient_bits++;
        quotient_bits += (__builtin_popcountll(nslots) > 1);   // Not a power of two.
        return quotient_bits + fp_len;
    }

    /**
     * Takes one off every count in one sweep of the entries, and evicts the
     * ones that reach zero.
     *
     * The evictions wait for the sweep to finish: removing an entry slides the
     * rest of its cluster down over the hole, which would move the slots the
     * sweep has yet to reach.
     *
     * @returns The number of entries evicted, i.e. the slots this freed.
     */
    uint64_t decrement_pass() {
        std::vector<std::pair<uint64_t, uint64_t>> victims;
        for (auto it = table_.begin(); it != table_.end(); ++it) {
            assert(sidecar_.counts[it.slot()] > 0);
            if (--sidecar_.counts[it.slot()] == 0)
                victims.push_back({it.bucket(), it.slot()});
        }
        total_decrements_++;
        table_.DeleteSlots(victims);
        return victims.size();
    }

    /** Puts `key` in at a count of one. */
    int32_t admit(uint64_t key, uint8_t flags) {
        const int64_t slot = table_.InsertAt(key, flags);
        if (slot < 0)
            return static_cast<int32_t>(slot);
        sidecar_.counts[slot] = 1;
        return 0;
    }
};

}   // namespace sublime
