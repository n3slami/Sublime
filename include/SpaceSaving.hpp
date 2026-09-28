#pragma once

/*
 * ============================================================================
 *
 *        SpaceSaving
 *          The Space-Saving summary over the Stream-Summary structure.
 *
 * ============================================================================
 *
 * Space-Saving (Metwally, Agarwal & El Abbadi, 2005) keeps a fixed number of
 * (element, count) monitors. While there is room a new element is admitted at
 * one; once full, a new element steals the *smallest* monitor -- taking over
 * its count and adding one, and inheriting that count as its over-estimation
 * error. An estimate is therefore always an over-estimate, and every element
 * whose true frequency exceeds the smallest monitored count is guaranteed to be
 * monitored.
 *
 * The classic data structure for it is the "Stream-Summary": monitors are
 * grouped into buckets by their shared count, the buckets held in a sorted
 * doubly-linked list, each bucket owning a doubly-linked list of its monitors,
 * and an index pointing from an element to its monitor. Every operation --
 * find, increment (move to the next bucket), and evict-the-minimum (the head
 * bucket) -- is then O(1). It is included here as a baseline precisely because
 * all those links make it pay a great deal of memory in pointers, which is the
 * cost `MG` and `SublimeMG` avoid by packing their state.
 *
 * ---------------------------------------------------------------------------
 * The index is a `CuckooTable`, not a hash map
 * ---------------------------------------------------------------------------
 * Which takes the most avoidable part of that memory away and makes the
 * comparison about the *structure* rather than about how the keys are stored:
 * the same fingerprints `MG` and `SublimeMG` index their monitored keys with,
 * at the same load factor, with a monitor pointer beside each slot. A monitor
 * no longer holds its key -- nothing does -- so what it holds instead is the
 * slot that names it, which the sidecar keeps true as kicks move entries about.
 *
 * Two consequences, both shared with `MG` and neither present when the index
 * was exact:
 *
 *  - **Two keys can share a monitor**, when their fingerprints and bucket pairs
 *    agree, and the estimate for both is then the sum. Space-Saving only ever
 *    over-estimates anyway, so this adds to an error that was already one-sided.
 *  - **An insertion can be turned away** when a kick path gives up. The
 *    occurrence is dropped, as it is in `MG` when a decrement frees nothing.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

#include "CuckooTable.hpp"

namespace sublime {

class SpaceSaving {
    friend class SpaceSavingTest;

public:
    /** The default length, in bits, of a fingerprint in the index. */
    static constexpr uint32_t default_fingerprint_length = 32;

    /**
     * How full the index is allowed to get. Far below what a cuckoo filter can
     * reach, and deliberately so: Space-Saving deletes and re-inserts on *every*
     * miss, so at a full table's load factor the kick paths are long, fail
     * often, and each failure drops a monitor with a large count on it -- 99% of
     * the counted mass, measured. Slack costs this baseline almost nothing,
     * since a slot is a few bytes against a monitor's 40 and a bucket's 32.
     */
    static constexpr double index_load_factor = 0.5;

    /**
     * @param capacity The number of monitors, i.e. how many keys it tracks.
     * @param seed The seed of the index's hash function.
     * @param fingerprint_length The length, in bits, of a stored fingerprint.
     */
    explicit SpaceSaving(uint64_t capacity, uint32_t seed = 1,
                         uint32_t fingerprint_length = default_fingerprint_length):
            capacity_{capacity},
            table_{slots_for(capacity),
                   CuckooTable::KeyBitsFor(slots_for(capacity), fingerprint_length),
                   CuckooTable::hashmode::Default, seed},
            sidecar_{table_.GetSlotCapacity()} {
        table_.AttachMirror(&sidecar_);
    }

    SpaceSaving(const SpaceSaving&) = delete;
    SpaceSaving& operator=(const SpaceSaving&) = delete;
    SpaceSaving(SpaceSaving&&) = delete;
    SpaceSaving& operator=(SpaceSaving&&) = delete;

    ~SpaceSaving() {
        clear();
    }

    /**
     * Counts one more occurrence of `key`: increments its monitor if it has
     * one, admits it at one if there is room, and otherwise takes over the
     * smallest monitor.
     */
    void Insert(uint64_t key, uint8_t /*flags*/ = 0) {
        n_++;
        const int64_t at = table_.FindMatch(key);
        if (at >= 0) {
            promote(sidecar_.monitors[at]);         // Already monitored.
            return;
        }
        if (CountMonitored() < capacity_) {         // Room to admit it.
            Counter *c = new Counter{0, 0, nullptr, nullptr, nullptr};
            if (!index(key, c)) {                   // The index had nowhere to put it.
                delete c;
                dropped_++;
                return;
            }
            attach_to_value(c, 1);
            return;
        }
        // Full: steal the smallest monitor. Its count becomes this key's error,
        // and this occurrence pushes the count one higher.
        Counter *victim = head_->child;             // Any monitor in the min bucket.
        unindex(victim);
        if (!index(key, victim)) {
            // The index turned the new key away, which a cuckoo filter can do
            // with room to spare. The monitor it was to take over is now
            // indexed by nothing, so it goes rather than being left adrift.
            lost_mass_ += victim->parent->value;
            detach(victim);
            delete victim;
            dropped_++;
            return;
        }
        victim->error = head_->value;
        promote(victim);
    }

    /**
     * @returns The estimated frequency of `key` -- the count on its monitor,
     * which is an over-estimate -- or 0 if it is not monitored.
     */
    uint64_t Query(uint64_t key, uint8_t /*flags*/ = 0) const {
        const int64_t at = table_.FindMatch(key);
        return at < 0 ? 0 : sidecar_.monitors[at]->parent->value;
    }

    /** @returns The over-estimation error on `key`'s monitor, or 0 if unmonitored. */
    uint64_t QueryError(uint64_t key) const {
        const int64_t at = table_.FindMatch(key);
        return at < 0 ? 0 : sidecar_.monitors[at]->error;
    }

    bool IsMonitored(uint64_t key, uint8_t /*flags*/ = 0) const {
        return table_.FindMatch(key) >= 0;
    }

    void FlushPrefetchQueue() {}

    void Reset() {
        clear();
        table_.Reset();             // Resets the sidecar with it.
        head_ = nullptr;
        n_ = 0;
        dropped_ = 0;
        lost_mass_ = 0;
    }

    uint64_t Capacity() const {
        return capacity_;
    }
    uint64_t CountMonitored() const {
        return table_.CountFingerprints();
    }
    uint64_t GetStreamLength() const {
        return n_;
    }
    /** Occurrences the index had nowhere to put, and so never counted. */
    uint64_t CountDroppedInsertions() const {
        return dropped_;
    }
    /**
     * The counts that went with monitors whose index entry a kick path dropped.
     * Between them, these two account for every occurrence the monitors do not:
     * `sum of counts + dropped + lost = N`.
     */
    uint64_t CountLostMass() const {
        return lost_mass_;
    }

    /** @returns The bytes held by the monitors, the buckets, and the index. */
    uint64_t SizeInBytes() const {
        return CountMonitored() * sizeof(Counter)
             + bucket_count_ * sizeof(Bucket)
             + table_.SizeInBytes()
             + sidecar_.monitors.size() * sizeof(Counter *);
    }
    uint64_t Size() const {
        return SizeInBytes();
    }

    /**
     * A conservative per-monitor byte cost -- a counter node, a bucket node
     * (the worst case of one bucket per monitor), a slot's fingerprint and the
     * pointer beside it -- for turning a byte budget into a monitor count.
     */
    static uint64_t BytesPerMonitorEstimate() {
        // A slot is the fingerprint plus its flag bit, and the monitor pointer
        // that shadows it, over the load factor the index runs at.
        const uint64_t slot_bytes = ((default_fingerprint_length + 1) / 8 + 1)
                                        + sizeof(Counter *);
        return sizeof(Counter) + sizeof(Bucket)
             + static_cast<uint64_t>(slot_bytes / index_load_factor);
    }

private:
    struct Bucket;

    /**
     * A monitor: the index slot that names it, its count's over-estimation
     * error, and its links.
     *
     * It no longer holds the key. The index is a cuckoo filter now, so the key
     * is not stored anywhere -- an eviction removes a *slot* -- and what the
     * monitor needs is the way back to that slot, which a kick can move.
     */
    struct Counter {
        uint64_t slot;
        uint64_t error;
        Bucket *parent;
        Counter *prev;      /**< Previous monitor in the bucket's child list. */
        Counter *next;      /**< Next monitor in the bucket's child list. */
    };

    /** A bucket: the count its monitors share, its neighbours, and its children. */
    struct Bucket {
        uint64_t value;
        Bucket *prev;       /**< Bucket with the next-smaller value. */
        Bucket *next;       /**< Bucket with the next-larger value. */
        Counter *child;     /**< Head of this bucket's list of monitors. */
    };

    /**
     * The monitor each slot of the index names, kept beside the fingerprints
     * the way `MG` keeps its counts: the table calls this wherever it places,
     * relocates, carries or clears an entry. A monitor's own `slot` follows,
     * so the two always agree.
     */
    struct Sidecar : CuckooTable::SlotMirror {
        std::vector<Counter *> monitors;
        Counter *held = nullptr;    /**< In hand while a kick path carries one. */

        explicit Sidecar(uint64_t nslots): monitors(nslots, nullptr) {}

        void MoveSlot(uint64_t from, uint64_t to) override {
            monitors[to] = monitors[from];
            if (monitors[to] != nullptr)
                monitors[to]->slot = to;
            monitors[from] = nullptr;
        }
        void SwapWithHeld(uint64_t slot) override {
            std::swap(monitors[slot], held);
            if (monitors[slot] != nullptr)
                monitors[slot]->slot = slot;
        }
        void ClearHeld() override {
            held = nullptr;
        }
        void Clear(uint64_t slot) override {
            monitors[slot] = nullptr;
        }
        void Reset() override {
            std::fill(monitors.begin(), monitors.end(), nullptr);
            held = nullptr;
        }
    };

    /** The slots an index needs to hold `capacity` keys at its load factor. */
    static uint64_t slots_for(uint64_t capacity) {
        return static_cast<uint64_t>(capacity / index_load_factor) + 1;
    }

    /**
     * Puts `key` in the index pointing at `c`, and tells `c` where it landed.
     *
     * @returns False if the index had nowhere to put it, which a cuckoo filter
     * can say with room to spare; the occurrence is then dropped, as it is in
     * `MG` when a decrement frees nothing.
     */
    bool index(uint64_t key, Counter *c) {
        const uint64_t lost_before = table_.CountLostEntries();
        const int64_t at = table_.InsertAt(key);
        if (at < 0)
            return false;
        sidecar_.monitors[at] = c;
        c->slot = at;
        if (table_.CountLostEntries() != lost_before) {
            // A kick path ran out of patience and dropped an older tenant, as a
            // cuckoo filter may. Nothing indexes that entry's monitor any more,
            // so it goes with it rather than sitting in the bucket lists
            // unreachable -- which is what the invariant check caught.
            Counter *orphan = sidecar_.held;
            sidecar_.held = nullptr;
            if (orphan != nullptr && orphan != c) {
                lost_mass_ += orphan->parent->value;
                detach(orphan);
                delete orphan;
            }
        }
        return true;
    }

    /** Takes `c`'s key out of the index, leaving the monitor itself alone. */
    void unindex(Counter *c) {
        table_.DeleteSlot(table_.BucketOfSlot(c->slot), c->slot);
    }

    uint64_t capacity_;
    Bucket *head_ = nullptr;    /**< The bucket with the smallest value. */
    uint64_t bucket_count_ = 0;
    uint64_t n_ = 0;
    uint64_t dropped_ = 0;      /**< Occurrences the index had no room for. */
    uint64_t lost_mass_ = 0;    /**< Counts that went with dropped entries. */
    CuckooTable table_;
    Sidecar sidecar_;

    /** Unlinks `c` from its bucket's child list, removing the bucket if emptied. */
    void detach(Counter *c) {
        Bucket *b = c->parent;
        if (c->prev != nullptr)
            c->prev->next = c->next;
        else
            b->child = c->next;
        if (c->next != nullptr)
            c->next->prev = c->prev;
        if (b->child == nullptr)
            remove_bucket(b);
    }

    /** Links `c` into the bucket of value `value`, creating it if need be. */
    void attach_to_value(Counter *c, uint64_t value) {
        // Find or make the bucket. `after` is the bucket the new one follows.
        Bucket *after = nullptr;
        Bucket *b = head_;
        while (b != nullptr && b->value < value) {
            after = b;
            b = b->next;
        }
        if (b == nullptr || b->value != value)
            b = insert_bucket_after(after, value);
        c->parent = b;
        c->prev = nullptr;
        c->next = b->child;
        if (b->child != nullptr)
            b->child->prev = c;
        b->child = c;
    }

    /** Increments `c`, moving it from its bucket to the one a count higher. */
    void promote(Counter *c) {
        Bucket *cur = c->parent;
        const uint64_t target_value = cur->value + 1;
        Bucket *next = cur->next;

        // Unlink from the current bucket without removing an emptied bucket yet:
        // the target may be the very bucket that follows, reached through it.
        if (c->prev != nullptr)
            c->prev->next = c->next;
        else
            cur->child = c->next;
        if (c->next != nullptr)
            c->next->prev = c->prev;

        Bucket *target = (next != nullptr && next->value == target_value)
                       ? next : insert_bucket_after(cur, target_value);
        c->parent = target;
        c->prev = nullptr;
        c->next = target->child;
        if (target->child != nullptr)
            target->child->prev = c;
        target->child = c;

        if (cur->child == nullptr)
            remove_bucket(cur);
    }

    /** Creates a bucket of value `value` just after `after` (or as the head). */
    Bucket *insert_bucket_after(Bucket *after, uint64_t value) {
        Bucket *next = after == nullptr ? head_ : after->next;
        Bucket *b = new Bucket{value, after, next, nullptr};
        if (after != nullptr)
            after->next = b;
        else
            head_ = b;
        if (next != nullptr)
            next->prev = b;
        bucket_count_++;
        return b;
    }

    void remove_bucket(Bucket *b) {
        if (b->prev != nullptr)
            b->prev->next = b->next;
        else
            head_ = b->next;
        if (b->next != nullptr)
            b->next->prev = b->prev;
        bucket_count_--;
        delete b;
    }

    void clear() {
        for (Bucket *b = head_; b != nullptr;) {
            for (Counter *c = b->child; c != nullptr;) {
                Counter *cn = c->next;
                delete c;
                c = cn;
            }
            Bucket *bn = b->next;
            delete b;
            b = bn;
        }
        head_ = nullptr;
        bucket_count_ = 0;
    }
};

}   // namespace sublime
