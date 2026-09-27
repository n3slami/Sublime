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
 * and a hash map pointing from an element to its monitor. Every operation --
 * find, increment (move to the next bucket), and evict-the-minimum (the head
 * bucket) -- is then O(1). It is included here as a baseline precisely because
 * all those links make it pay a great deal of memory in pointers, which is the
 * cost `MG` and `SublimeMG` avoid by packing their state.
 *
 * Full 64-bit keys are stored, with no fingerprinting.
 */

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace sublime {

class SpaceSaving {
    friend class SpaceSavingTest;

public:
    /** @param capacity The number of monitors, i.e. how many keys it tracks. */
    explicit SpaceSaving(uint64_t capacity): capacity_{capacity} {}

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
        auto it = index_.find(key);
        if (it != index_.end()) {
            promote(it->second);                    // Already monitored.
            return;
        }
        if (index_.size() < capacity_) {            // Room to admit it.
            Counter *c = new Counter{key, 0, nullptr, nullptr, nullptr};
            attach_to_value(c, 1);
            index_[key] = c;
            return;
        }
        // Full: steal the smallest monitor. Its count becomes this key's error,
        // and this occurrence pushes the count one higher.
        Counter *victim = head_->child;             // Any monitor in the min bucket.
        index_.erase(victim->elem);
        victim->elem = key;
        victim->error = head_->value;
        index_[key] = victim;
        promote(victim);
    }

    /**
     * @returns The estimated frequency of `key` -- the count on its monitor,
     * which is an over-estimate -- or 0 if it is not monitored.
     */
    uint64_t Query(uint64_t key, uint8_t /*flags*/ = 0) const {
        auto it = index_.find(key);
        return it == index_.end() ? 0 : it->second->parent->value;
    }

    /** @returns The over-estimation error on `key`'s monitor, or 0 if unmonitored. */
    uint64_t QueryError(uint64_t key) const {
        auto it = index_.find(key);
        return it == index_.end() ? 0 : it->second->error;
    }

    bool IsMonitored(uint64_t key, uint8_t /*flags*/ = 0) const {
        return index_.find(key) != index_.end();
    }

    void FlushPrefetchQueue() {}

    void Reset() {
        clear();
        index_.clear();
        head_ = nullptr;
        n_ = 0;
    }

    uint64_t Capacity() const {
        return capacity_;
    }
    uint64_t CountMonitored() const {
        return index_.size();
    }
    uint64_t GetStreamLength() const {
        return n_;
    }

    /** @returns The bytes held by the monitors, the buckets, and the index. */
    uint64_t SizeInBytes() const {
        return index_.size() * sizeof(Counter)
             + bucket_count_ * sizeof(Bucket)
             + index_.size() * (sizeof(uint64_t) + sizeof(void *)) * 2;   // map, ~2x load.
    }
    uint64_t Size() const {
        return SizeInBytes();
    }

    /**
     * A conservative per-monitor byte cost -- a counter node, a bucket node
     * (the worst case of one bucket per monitor), and an index entry at a
     * generous load factor -- for turning a byte budget into a monitor count.
     */
    static uint64_t BytesPerMonitorEstimate() {
        return sizeof(Counter) + sizeof(Bucket) + 2 * (sizeof(uint64_t) + sizeof(void *));
    }

private:
    struct Bucket;

    /** A monitor: an element, its count's over-estimation error, and its links. */
    struct Counter {
        uint64_t elem;
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

    uint64_t capacity_;
    Bucket *head_ = nullptr;    /**< The bucket with the smallest value. */
    uint64_t bucket_count_ = 0;
    uint64_t n_ = 0;
    std::unordered_map<uint64_t, Counter *> index_;

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
