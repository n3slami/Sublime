#pragma once

/*
 * ============================================================================
 *
 *        MGHeap
 *          A classic heap-based Misra-Gries summary, storing fingerprints.
 *
 * ============================================================================
 *
 * The textbook heap-based Misra-Gries: a bounded set of monitored keys, each
 * with a count, a global `lazy_decrement_` standing in for the decrements owed,
 * and a min-heap to find the smallest count on an overflow. The only twist is
 * where the monitored set lives -- in a fixed-size RSQF `FingerprintTable`
 * storing one fixed-length fingerprint per key, rather than a hash map of full
 * keys -- so its per-key footprint is comparable to `SublimeMG`'s. This makes
 * it the fair, fingerprint-storing baseline for `SublimeMG`.
 *
 * Unlike `SublimeMG`, the table never expands: every fingerprint is the same
 * fixed length, so a key matches at most one entry (`FindLongestMatch` returns
 * a single slot, and there are no chains). Two keys sharing a fingerprint are
 * merged into one entry, which only ever over-estimates -- exactly the trade
 * `SublimeMG` makes.
 *
 * ---------------------------------------------------------------------------
 * Keeping the heap addresses stable under RSQF shifts
 * ---------------------------------------------------------------------------
 * An RSQF slides a whole run of slots along whenever it inserts or deletes, so
 * a heap that referred to entries by their physical slot would need patching on
 * every shift. Instead a heap entry is a `(count, home_bucket)` pair: the home
 * bucket is a key's quotient, invariant while the table is not resized, so the
 * heap array is never touched by a shift.
 *
 * To go the other way -- from a monitored entry to its place in the heap, so an
 * incremented count can be sifted -- each slot carries a `heap_offset`: the
 * index of its entry in the heap. Both the counts and these offsets are held in
 * a `SlotMirror` sidecar the table shifts in lockstep with its fingerprints, so
 * they stay attached to the right key across every shift. The counts are a
 * plain 32-bit array; the offsets are packed to just the bits an index needs.
 *
 * Resolving a heap position back to a slot (needed when a sift swaps two heap
 * entries, and when the root is evicted) is the one place that costs more than
 * O(1): the entry's short run is scanned for the slot whose `heap_offset`
 * matches. That is the deliberate price of heap addresses that survive shifts.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

#include "FingerprintTable.hpp"

namespace sublime {

/**
 * A flat array of fixed-width unsigned fields packed contiguously into 64-bit
 * words, for the per-slot heap offsets. Field width is only as wide as a heap
 * index needs to be.
 */
class PackedArray {
public:
    PackedArray() = default;
    PackedArray(uint64_t size, uint32_t width): size_{size}, width_{width} {
        assert(width_ > 0 && width_ <= 63);
        // One guard word so a field straddling the last boundary can be read
        // and written with an unconditional two-word access.
        words_.assign((size_ * width_ + 63) / 64 + 1, 0);
    }

    uint64_t Get(uint64_t index) const {
        const uint64_t bit = index * width_;
        const uint64_t w = bit >> 6, off = bit & 63;
        uint64_t v = words_[w] >> off;
        if (off + width_ > 64)
            v |= words_[w + 1] << (64 - off);
        return v & mask();
    }

    void Set(uint64_t index, uint64_t value) {
        value &= mask();
        const uint64_t bit = index * width_;
        const uint64_t w = bit >> 6, off = bit & 63;
        words_[w] = (words_[w] & ~(mask() << off)) | (value << off);
        if (off + width_ > 64) {
            const uint32_t low = 64 - off;
            words_[w + 1] = (words_[w + 1] & ~(mask() >> low)) | (value >> low);
        }
    }

    void Clear() {
        std::fill(words_.begin(), words_.end(), 0);
    }

    uint64_t SizeInBytes() const {
        return (size_ * width_ + 7) / 8;
    }

private:
    uint64_t mask() const {
        return width_ == 64 ? ~0ULL : ((1ULL << width_) - 1);
    }

    std::vector<uint64_t> words_;
    uint64_t size_ = 0;
    uint32_t width_ = 0;
};


class MGHeap {
    friend class MGHeapTest;

public:
    using hashmode = FingerprintTable::hashmode;

    /** Signals that the key passed in has already been hashed. */
    static constexpr uint32_t flag_key_is_hash = FingerprintTable::flag_key_is_hash;
    static constexpr int32_t err_no_space = FingerprintTable::err_no_space;

    /** The default length, in bits, of a stored fingerprint. */
    static constexpr uint32_t default_fingerprint_length = 10;

    /**
     * @param nslots The number of slots the fingerprint table holds. `Capacity`
     * is a shade under this.
     * @param hash_mode The hashing mode, see `FingerprintTable::hashmode`.
     * @param seed The seed of the hash function.
     * @param fingerprint_length The length, in bits, of each stored fingerprint.
     */
    MGHeap(uint64_t nslots, hashmode hash_mode, uint32_t seed,
           uint32_t fingerprint_length = default_fingerprint_length):
            table_{nslots, key_bits_for(nslots, fingerprint_length), hash_mode, seed},
            sidecar_{table_.GetSlotCapacity(), offset_bits_for(capacity_for(nslots))} {
        table_.AttachMirror(&sidecar_);
        heap_.reserve(capacity_for(nslots) + 1);
    }

    // The table stores a raw pointer to `sidecar_`, so the object must not move.
    MGHeap(const MGHeap&) = delete;
    MGHeap& operator=(const MGHeap&) = delete;
    MGHeap(MGHeap&&) = delete;
    MGHeap& operator=(MGHeap&&) = delete;

    /**
     * Counts one more occurrence of `key`, applying whichever Misra-Gries case
     * fits: an already-monitored key's count goes up, a key with room to spare
     * is admitted at one, and otherwise every count comes down by one (via the
     * lazy decrement), anything that reaches zero is evicted, and this
     * occurrence is dropped.
     *
     * @returns 0, or `err_no_space`.
     */
    int32_t Insert(uint64_t key, uint8_t flags = 0) {
        n_++;
        const int64_t slot = table_.FindLongestMatch(key, flags);
        if (slot >= 0) {                            // Already monitored.
            increment(static_cast<uint64_t>(slot));
            return 0;
        }
        if (CountMonitored() < Capacity())          // Room to admit it.
            return admit(key, flags);

        // Full: the Misra-Gries decrement. The occurrence itself is dropped.
        lazy_decrement_++;
        total_decrements_++;
        evict_at_or_below(lazy_decrement_);
        return 0;
    }

    /**
     * Estimates the frequency of `key`: the count stored on the single
     * fingerprint matching it, less the lazy decrement, or 0 if nothing
     * matches. A fingerprint collision can only make this an over-estimate.
     */
    uint64_t Query(uint64_t key, uint8_t flags = 0) const {
        const int64_t slot = table_.FindLongestMatch(key, flags);
        if (slot < 0)
            return 0;
        const uint64_t stored = sidecar_.counts[slot];
        return stored > lazy_decrement_ ? stored - lazy_decrement_ : 0;
    }

    /** @returns True if `key` is monitored. */
    bool IsMonitored(uint64_t key, uint8_t flags = 0) const {
        return table_.FindLongestMatch(key, flags) >= 0;
    }

    /** A no-op, for interface parity with the other sketches' prefetch queues. */
    void FlushPrefetchQueue() {}

    /** Empties the summary, keeping its shape. */
    void Reset() {
        table_.Reset();     // Resets the sidecar with it.
        heap_.clear();
        lazy_decrement_ = 0;
        total_decrements_ = 0;
        n_ = 0;
    }

    /* Size and shape. */

    /** @returns How many keys the summary can monitor at once. */
    uint64_t Capacity() const {
        return static_cast<uint64_t>(table_.CountSlots() * FingerprintTable::max_load_factor);
    }
    /** @returns The number of keys currently monitored. */
    uint64_t CountMonitored() const {
        return table_.CountFingerprints();
    }
    uint64_t GetLazyDecrement() const {
        return lazy_decrement_;
    }
    uint64_t CountDecrements() const {
        return total_decrements_;
    }
    /** @returns `N`, the number of insertions the summary has seen. */
    uint64_t GetStreamLength() const {
        return n_;
    }
    /** @returns The bytes held by the fingerprints, counters, offsets, and heap. */
    uint64_t SizeInBytes() const {
        return table_.SizeInBytes()
             + sidecar_.counts.capacity() * sizeof(uint32_t)
             + sidecar_.offsets.SizeInBytes()
             + heap_.capacity() * sizeof(HeapEntry);
    }
    /** Alias used by the benchmark harness. */
    uint64_t Size() const {
        return SizeInBytes();
    }

    const FingerprintTable& Table() const {
        return table_;
    }

private:
    /** A monitored entry as the heap sees it: its count, and its stable home bucket. */
    struct HeapEntry {
        uint32_t count;
        uint32_t bucket;
    };

    /**
     * The per-slot data the table keeps aligned to its fingerprints: a count
     * and the entry's index in the heap. Both follow their fingerprint through
     * every slot shift.
     */
    struct Sidecar : FingerprintTable::SlotMirror {
        std::vector<uint32_t> counts;
        PackedArray offsets;

        Sidecar(uint64_t nslots_alloc, uint32_t offset_bits):
                counts(nslots_alloc, 0), offsets(nslots_alloc, offset_bits) {}

        // Net effect of the RSQF closing a hole: [hole, last) take their right
        // neighbour, and `last` is cleared.
        void ShiftLeftAndClear(uint64_t hole, uint64_t last) override {
            for (uint64_t i = hole; i < last; i++) {
                counts[i] = counts[i + 1];
                offsets.Set(i, offsets.Get(i + 1));
            }
            counts[last] = 0;
            offsets.Set(last, 0);
        }

        // Net effect of the RSQF opening a hole: (hole, last] take their left
        // neighbour, and `hole` is cleared.
        void ShiftRightAndClear(uint64_t hole, uint64_t last) override {
            for (uint64_t i = last; i > hole; i--) {
                counts[i] = counts[i - 1];
                offsets.Set(i, offsets.Get(i - 1));
            }
            counts[hole] = 0;
            offsets.Set(hole, 0);
        }

        void Reset() override {
            std::fill(counts.begin(), counts.end(), 0);
            offsets.Clear();
        }
    };

    FingerprintTable table_;
    Sidecar sidecar_;
    std::vector<HeapEntry> heap_;
    /** What every stored count owes; a query subtracts it once. */
    uint64_t lazy_decrement_ = 0;
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

    static uint64_t capacity_for(uint64_t nslots) {
        return static_cast<uint64_t>(nslots * FingerprintTable::max_load_factor);
    }

    /** Just enough bits to index a heap of up to `capacity` entries. */
    static uint32_t offset_bits_for(uint64_t capacity) {
        const uint64_t c = capacity < 1 ? 1 : capacity;
        return static_cast<uint32_t>(64 - __builtin_clzll(c));
    }

    uint64_t get_offset(uint64_t slot) const {
        return sidecar_.offsets.Get(slot);
    }
    void set_offset(uint64_t slot, uint64_t pos) {
        sidecar_.offsets.Set(slot, pos);
    }

    /**
     * @returns The slot of the entry sitting at heap position `pos`, whose home
     * bucket is `bucket`. Scans `bucket`'s run for the matching `heap_offset`.
     */
    int64_t resolve_slot(uint64_t bucket, uint64_t pos) const {
        int64_t found = -1;
        table_.ForEachSlotInRun(bucket, [&](uint64_t s) {
            if (found < 0 && get_offset(s) == pos)
                found = static_cast<int64_t>(s);
        });
        return found;
    }

    /** Case 1: the key is already monitored, so its count rises. */
    void increment(uint64_t slot) {
        sidecar_.counts[slot]++;
        const uint64_t pos = get_offset(slot);
        heap_[pos].count = sidecar_.counts[slot];
        sift_down(pos, slot);       // A larger count sinks in a min-heap.
    }

    /** Case 2: put `key` in at a count of `lazy_decrement_ + 1` (i.e. one). */
    int32_t admit(uint64_t key, uint8_t flags) {
        const int64_t slot = table_.InsertAt(key, flags);
        if (slot < 0)
            return static_cast<int32_t>(slot);
        const uint32_t stored = static_cast<uint32_t>(lazy_decrement_ + 1);
        sidecar_.counts[slot] = stored;
        const uint64_t pos = heap_.size();
        heap_.push_back({stored, static_cast<uint32_t>(table_.HomeBucket(key, flags))});
        set_offset(static_cast<uint64_t>(slot), pos);
        sift_up(pos, static_cast<uint64_t>(slot));
        return 0;
    }

    /** Evicts every entry whose count has fallen to `threshold` or below. */
    void evict_at_or_below(uint64_t threshold) {
        while (!heap_.empty() && heap_[0].count <= threshold) {
            const uint64_t bucket = heap_[0].bucket;
            const int64_t evicted_slot = resolve_slot(bucket, 0);
            assert(evicted_slot >= 0);
            heap_pop_root();
            table_.DeleteSlot(bucket, static_cast<uint64_t>(evicted_slot));
        }
    }

    /** Removes the root, moving the last entry up and sifting it down. */
    void heap_pop_root() {
        const uint64_t last = heap_.size() - 1;
        if (last == 0) {
            heap_.pop_back();
            return;
        }
        const int64_t moved_slot = resolve_slot(heap_[last].bucket, last);
        assert(moved_slot >= 0);
        heap_[0] = heap_[last];
        heap_.pop_back();
        set_offset(static_cast<uint64_t>(moved_slot), 0);
        sift_down(0, static_cast<uint64_t>(moved_slot));
    }

    /** Sifts the entry at `pos` (whose slot is `slot`) toward the root. */
    void sift_up(uint64_t pos, uint64_t slot) {
        while (pos > 0) {
            const uint64_t parent = (pos - 1) / 2;
            if (heap_[parent].count <= heap_[pos].count)
                break;
            const int64_t parent_slot = resolve_slot(heap_[parent].bucket, parent);
            assert(parent_slot >= 0);
            std::swap(heap_[pos], heap_[parent]);
            set_offset(slot, parent);
            set_offset(static_cast<uint64_t>(parent_slot), pos);
            pos = parent;
        }
    }

    /** Sifts the entry at `pos` (whose slot is `slot`) toward the leaves. */
    void sift_down(uint64_t pos, uint64_t slot) {
        const uint64_t n = heap_.size();
        while (true) {
            const uint64_t left = 2 * pos + 1, right = 2 * pos + 2;
            uint64_t smallest = pos;
            if (left < n && heap_[left].count < heap_[smallest].count)
                smallest = left;
            if (right < n && heap_[right].count < heap_[smallest].count)
                smallest = right;
            if (smallest == pos)
                break;
            const int64_t child_slot = resolve_slot(heap_[smallest].bucket, smallest);
            assert(child_slot >= 0);
            std::swap(heap_[pos], heap_[smallest]);
            set_offset(slot, smallest);
            set_offset(static_cast<uint64_t>(child_slot), pos);
            pos = smallest;
        }
    }
};

}   // namespace sublime
