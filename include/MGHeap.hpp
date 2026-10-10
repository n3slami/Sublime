#pragma once

/*
 * ============================================================================
 *
 *        MGHeap
 *          A heap-based Misra-Gries summary, storing fingerprints.
 *
 * ============================================================================
 *
 * The textbook heap-based Misra-Gries: a bounded set of monitored keys, a lazy
 * decrement `L` standing in for every decrement owed, and a min-heap of the
 * counts, so that a decrement is `L++` and finding what it emptied is a look
 * at the root. A count is stored as its value plus `L`, which is what the heap
 * orders, and reaches zero exactly when the root catches up with `L`.
 *
 * The monitored set is the same fixed-size `CuckooTable` `MG` uses, and the
 * algorithm is the one `MG` and `SublimeMG` run: an occurrence that finds the
 * summary full costs every count one, and takes the slot of a count that
 * reached zero -- or is dropped, if none did.
 *
 * ---------------------------------------------------------------------------
 * Eviction is drained, not batched
 * ---------------------------------------------------------------------------
 * A decrement can empty a large share of the summary at once, and evicting all
 * of it on the spot is the worst insertion a heap makes: `k` pops of `log w`
 * each, `k` running to a sizeable fraction of `w`. That is worse than `MG`'s
 * `O(w)` sweep, and it scales with the summary.
 *
 * So an emptied entry -- a stored count of exactly `L` -- is left where it is,
 * and **every insertion evicts at most `drain_batch` of them on its way out**,
 * whichever case it fell into, as `SublimeMG`'s min-tree configuration does.
 * Every emptied entry is still evicted, a couple per insertion, and none
 * waits on the arrival that needs a slot. An emptied entry costs no accuracy
 * meanwhile: it answers zero, which is what an unmonitored key answers.
 *
 * Two things keep the algorithm Misra-Gries'. An arrival that finds the table
 * full with an emptied entry still in it takes that entry's slot and decrements
 * nothing, which is what it would have found had the entry been evicted at
 * once; so `L` only rises when nothing is at zero, and no stored count ever
 * falls below `L`. And a key whose emptied entry is still stored, arriving
 * again, finds it and goes from zero to one, as re-admission would.
 *
 * So the summary holds the same keys at the same counts as `MG`'s -- but not in
 * the same slots, since the emptied entries it keeps a while are slots the
 * table cannot place anything in. That changes nothing until a kick path gives
 * up, which it now can where `MG`'s does not, and the reverse; from there the
 * two drift apart.
 *
 * ---------------------------------------------------------------------------
 * Offsets, not pointers
 * ---------------------------------------------------------------------------
 * A classic implementation links a hash-table entry and its heap node with a
 * pointer each way. Here both links are *offsets*, packed to just the bits
 * they need:
 *
 *  - a heap node holds its entry's **slot** in the table, `ceil(log2 nslots)`
 *    bits, beside its 32-bit count;
 *  - a table slot holds its entry's **position** in the heap, `ceil(log2 w)`
 *    bits for a heap of `w = Capacity()` nodes.
 *
 * The counts live in the heap alone, so a query reads the slot's position and
 * then the count there. A sift moves a node, and every node it moves has its
 * slot's position rewritten, which is what keeps the second link right; a
 * cuckoo kick moves an entry between slots, and the table's `SlotMirror`
 * callbacks carry its position along and rewrite its node's slot, which keeps
 * the first. Nothing ever has to be searched for.
 *
 * ---------------------------------------------------------------------------
 * The arrival, and what a kick path can lose
 * ---------------------------------------------------------------------------
 * An admission pushes its node *before* asking the table for a slot, so that
 * the arrival has a position to carry if the table has to kick: a kick path
 * starts with the arrival in hand (`ClearHeld`), and every step swaps the hand
 * with a slot. Two things can then go wrong, both of which `MG` meets too:
 *
 *  - the arrival is turned away (`err_no_space`), and its node, still the last
 *    one and never sifted, is popped;
 *  - an older tenant is left in hand when the kicks run out, and the table
 *    drops it. Its position is what the hand holds, so its node is removed
 *    from the heap -- the count it carried is lost, as it is in `MG`.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

#include "CuckooTable.hpp"

namespace sublime {

/**
 * A flat array of fixed-width unsigned fields packed contiguously into 64-bit
 * words, for the offsets linking the table and the heap. A field is only as
 * wide as the index it holds needs to be.
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
        assert(value <= mask());
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

    uint32_t Width() const {
        return width_;
    }

    uint64_t SizeInBytes() const {
        return (size_ * width_ + 7) / 8;
    }

private:
    uint64_t mask() const {
        return (1ULL << width_) - 1;
    }

    std::vector<uint64_t> words_;
    uint64_t size_ = 0;
    uint32_t width_ = 0;
};


/**
 * @tparam Table The monitored-key set, which is `CuckooTable` -- the same one
 * `MG` and `SublimeMG` use.
 */
template <typename Table = CuckooTable>
class MGHeap {
    friend class MGHeapTest;

public:
    /**
     * The most emptied entries an insertion evicts on its way out. Two keeps
     * ahead of the one an arrival may take for itself; `SublimeMG` measured 1,
     * 2, 4 and 16 the same, so it is not a knob to tune.
     */
    static constexpr uint32_t drain_batch = 2;

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
     * @param hash_mode The hashing mode, see `CuckooTable::hashmode`.
     * @param seed The seed of the hash function.
     * @param fingerprint_length The length, in bits, of each stored fingerprint.
     */
    MGHeap(uint64_t nslots, hashmode hash_mode, uint32_t seed,
           uint32_t fingerprint_length = default_fingerprint_length):
            table_{nslots, Table::KeyBitsFor(nslots, fingerprint_length), hash_mode, seed},
            capacity_{static_cast<uint64_t>(table_.CountSlots() * Table::max_load_factor)},
            counts_(capacity_, 0),
            slots_{capacity_ < 1 ? 1 : capacity_, bits_for(table_.GetSlotCapacity())},
            sidecar_{table_.GetSlotCapacity(), bits_for(capacity_), &slots_} {
        table_.AttachMirror(&sidecar_);
    }

    // The table stores a raw pointer to `sidecar_`, and the sidecar one to
    // `slots_`, so the object must not move.
    MGHeap(const MGHeap&) = delete;
    MGHeap& operator=(const MGHeap&) = delete;
    MGHeap(MGHeap&&) = delete;
    MGHeap& operator=(MGHeap&&) = delete;

    /**
     * Counts one more occurrence of `key`, applying whichever Misra-Gries case
     * fits: an already-monitored key's count goes up, a key with room to spare
     * is admitted at one, and otherwise it takes the slot of an emptied entry
     * -- after a decrement (`L++`), if nothing was at zero already -- or is
     * dropped, if the decrement emptied nothing. Then up to `drain_batch`
     * emptied entries are evicted.
     *
     * @returns 0, or `err_no_space`.
     */
    int32_t Insert(uint64_t key, uint8_t flags = 0) {
        n_++;
        const int32_t status = insert(key, flags);
        drain_emptied();
        return status;
    }

    /**
     * Estimates the frequency of `key`: the count of the single entry matching
     * it, or 0 if nothing matches. A fingerprint collision can only make this
     * an over-estimate.
     */
    uint64_t Query(uint64_t key, uint8_t flags = 0) const {
        const int64_t slot = table_.FindMatch(key, flags);
        if (slot < 0)
            return 0;
        const uint32_t stored = counts_[sidecar_.positions.Get(static_cast<uint64_t>(slot))];
        assert(stored >= lazy_decrement_);
        return stored - lazy_decrement_;    // Zero for an emptied entry not yet drained.
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
        heap_size_ = 0;
        lazy_decrement_ = 0;
        total_decrements_ = 0;
        n_ = 0;
    }

    /* Size and shape. */

    /** @returns How many keys the summary can monitor at once. */
    uint64_t Capacity() const {
        return capacity_;
    }
    /**
     * @returns The number of entries stored, counting the emptied ones the
     * drain has yet to evict -- which is what decides whether the table has room.
     */
    uint64_t CountMonitored() const {
        return table_.CountFingerprints();
    }
    /**
     * @returns The entries with a count above zero, i.e. what `MG` would call
     * monitored. The emptied entries are the nodes stored at exactly `L`, which
     * all sit in a subtree at the root, so this costs a walk of that subtree.
     */
    uint64_t CountLive() const {
        uint64_t emptied = 0;
        std::vector<uint64_t> stack;
        if (heap_size_ > 0 && counts_[0] <= lazy_decrement_)
            stack.push_back(0);
        while (!stack.empty()) {
            const uint64_t pos = stack.back();
            stack.pop_back();
            emptied++;
            for (uint64_t child = 2 * pos + 1; child <= 2 * pos + 2 && child < heap_size_; child++)
                if (counts_[child] <= lazy_decrement_)
                    stack.push_back(child);
        }
        return heap_size_ - emptied;
    }
    /**
     * @returns How many times every count came down by one. This is the
     * classic Misra-Gries parameter: no key's count is understated by more
     * than this.
     */
    uint64_t CountDecrements() const {
        return total_decrements_;
    }
    /** @returns `L`, what every stored count owes. */
    uint64_t GetLazyDecrement() const {
        return lazy_decrement_;
    }
    /** @returns `N`, the number of insertions the summary has seen. */
    uint64_t GetStreamLength() const {
        return n_;
    }
    /**
     * @returns The bytes held by the fingerprints, the heap's counts and slot
     * offsets, and the table's heap offsets.
     */
    uint64_t SizeInBytes() const {
        return table_.SizeInBytes()
             + counts_.capacity() * sizeof(uint32_t)
             + slots_.SizeInBytes()
             + sidecar_.positions.SizeInBytes();
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
     * Each slot's position in the heap, which the table keeps aligned to its
     * fingerprints. Every callback that lands an entry in a slot also rewrites
     * that entry's heap node to name the slot, so both links follow a kick.
     */
    struct Sidecar : Table::SlotMirror {
        PackedArray positions;
        /** The heap's slot offsets, which a landing entry's node is rewritten in. */
        PackedArray *heap_slots;
        /** The position in hand while a kick path carries an entry. */
        uint64_t held = 0;
        /** The arrival's node, which a kick path starts out carrying. */
        uint64_t arrival = 0;

        Sidecar(uint64_t nslots_alloc, uint32_t position_bits, PackedArray *slots):
                positions(nslots_alloc, position_bits), heap_slots{slots} {}

        void MoveSlot(uint64_t from, uint64_t to) override {
            const uint64_t pos = positions.Get(from);
            positions.Set(to, pos);
            heap_slots->Set(pos, to);
        }

        /**
         * The hand always holds a live entry when this is called -- the arrival
         * first, then whatever the previous step displaced -- so its node is
         * real and is told where it landed. What comes back out of `slot` is
         * either another live entry or, on the step that finds a free slot,
         * whatever a vacant slot held, which nothing reads.
         */
        void SwapWithHeld(uint64_t slot) override {
            const uint64_t there = positions.Get(slot);
            positions.Set(slot, held);
            heap_slots->Set(held, slot);
            held = there;
        }

        void ClearHeld() override {
            held = arrival;
        }

        /** A vacant slot's position is never read, so there is nothing to clear. */
        void Clear(uint64_t) override {}

        void Reset() override {
            positions.Clear();
            held = 0;
        }
    };

    Table table_;
    uint64_t capacity_;
    /** The heap's counts, each its value plus `lazy_decrement_`. */
    std::vector<uint32_t> counts_;
    /** The heap's slot offsets: node `i` is the entry in slot `slots_[i]`. */
    PackedArray slots_;
    Sidecar sidecar_;
    uint64_t heap_size_ = 0;
    /** What every stored count owes; a query subtracts it once. */
    uint64_t lazy_decrement_ = 0;
    uint64_t total_decrements_ = 0;
    uint64_t n_ = 0;

    /** Just enough bits to hold any index below `n`. */
    static uint32_t bits_for(uint64_t n) {
        return n <= 2 ? 1 : msb64_nonzero(n - 1) + 1;
    }

    /** Points node `pos` and slot `slot` at each other. */
    void link(uint64_t pos, uint64_t slot) {
        slots_.Set(pos, slot);
        sidecar_.positions.Set(slot, pos);
    }

    /**
     * Puts `key` in at a count of one, i.e. a stored `L + 1`.
     *
     * The node goes on the heap first, so that a kick path has the arrival's
     * position to carry; see the file comment for what comes back.
     */
    int32_t admit(uint64_t key, uint8_t flags) {
        assert(heap_size_ < capacity_);
        assert(lazy_decrement_ + 1 <= UINT32_MAX);
        const uint64_t pos = heap_size_++;
        counts_[pos] = static_cast<uint32_t>(lazy_decrement_ + 1);
        sidecar_.arrival = pos;

        const uint64_t lost_before = table_.CountLostEntries();
        const int64_t slot = table_.InsertAt(key, flags);
        if (slot < 0) {                             // Turned away: nothing moved for good.
            assert(pos == heap_size_ - 1);
            heap_size_--;
            return static_cast<int32_t>(slot);
        }
        link(pos, static_cast<uint64_t>(slot));
        // A tenant fell off the kick path. Its node goes first, while the
        // position in hand still names it: sifting the arrival could move it.
        // The arrival is the last node, so removing the tenant's moves the
        // arrival into its place and sifts it there.
        if (table_.CountLostEntries() != lost_before)
            remove(sidecar_.held);
        sift_up(sidecar_.positions.Get(static_cast<uint64_t>(slot)));
        return 0;
    }

    /** The three Misra-Gries cases, without the drain. */
    int32_t insert(uint64_t key, uint8_t flags) {
        const int64_t slot = table_.FindMatch(key, flags);
        if (slot >= 0) {                            // Already stored, emptied or not.
            const uint64_t pos = sidecar_.positions.Get(static_cast<uint64_t>(slot));
            assert(counts_[pos] < UINT32_MAX);
            counts_[pos]++;
            sift_down(pos);                         // A larger count sinks in a min-heap.
            return 0;
        }
        if (CountMonitored() < Capacity())          // Room to admit it.
            return admit(key, flags);

        // Full. An entry already at zero gives up its slot; failing that, the
        // Misra-Gries decrement, and the occurrence that paid for it takes the
        // slot of an entry it emptied, if it emptied any.
        //
        // With a drain of two the first test never finds one: an insertion adds
        // at most one entry and drains two, so the table shrinks while emptied
        // entries are left and is never full again until they are gone. It is
        // what keeps this Misra-Gries for a drain of one, which would not.
        if (!root_is_emptied()) {
            lazy_decrement_++;
            total_decrements_++;
            if (!root_is_emptied())
                return 0;
        }
        evict_root();
        return admit(key, flags);
    }

    /** @returns True if the smallest count is zero. */
    bool root_is_emptied() const {
        return heap_size_ > 0 && counts_[0] <= lazy_decrement_;
    }

    /** Evicts the entry at the root of the heap. */
    void evict_root() {
        const uint64_t slot = slots_.Get(0);
        remove(0);
        table_.DeleteSlot(table_.BucketOfSlot(slot), slot);
    }

    /** Evicts up to `drain_batch` emptied entries, all of them at the root in turn. */
    void drain_emptied() {
        for (uint32_t i = 0; i < drain_batch && root_is_emptied(); i++)
            evict_root();
    }

    /** Takes node `pos` out of the heap, filling its place with the last node. */
    void remove(uint64_t pos) {
        assert(pos < heap_size_);
        const uint64_t last = --heap_size_;
        if (pos == last)
            return;
        counts_[pos] = counts_[last];
        link(pos, slots_.Get(last));
        if (pos > 0 && counts_[(pos - 1) / 2] > counts_[pos])
            sift_up(pos);
        else
            sift_down(pos);
    }

    /**
     * Sifts node `pos` toward the root. The node is carried and written once,
     * where it stops; each node it passes moves down one level and has its
     * slot told where it went.
     */
    void sift_up(uint64_t pos) {
        const uint32_t count = counts_[pos];
        const uint64_t slot = slots_.Get(pos);
        while (pos > 0) {
            const uint64_t parent = (pos - 1) / 2;
            if (counts_[parent] <= count)
                break;
            counts_[pos] = counts_[parent];
            link(pos, slots_.Get(parent));
            pos = parent;
        }
        counts_[pos] = count;
        link(pos, slot);
    }

    /** Sifts node `pos` toward the leaves, the same way. */
    void sift_down(uint64_t pos) {
        const uint32_t count = counts_[pos];
        const uint64_t slot = slots_.Get(pos);
        while (true) {
            uint64_t child = 2 * pos + 1;
            if (child >= heap_size_)
                break;
            if (child + 1 < heap_size_ && counts_[child + 1] < counts_[child])
                child++;
            if (counts_[child] >= count)
                break;
            counts_[pos] = counts_[child];
            link(pos, slots_.Get(child));
            pos = child;
        }
        counts_[pos] = count;
        link(pos, slot);
    }
};

}   // namespace sublime
