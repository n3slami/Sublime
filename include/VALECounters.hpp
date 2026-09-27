#pragma once

/*
 * ============================================================================
 *
 *        VALECounters
 *          A flat array of variable-length counters, laid out with VALE
 *          exactly as `SublimeCMS` lays out its sketch. It is the counter
 *          half of Sublime_MG: counter `i` holds the count of the fingerprint
 *          `CuckooTable` stores in slot `i`.
 *
 * ============================================================================
 *
 * ---------------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------------
 * The counters are cut into *chunks* of one cache line each, holding
 * `counters_per_chunk` counters apiece. Within a chunk, from the low bit up:
 *
 *      [ overflows bitmap ][ stubs ] ... free ... [ extension pool ]
 *          counters_per_chunk bits, one per counter
 *                          counters_per_chunk * stub_size bits
 *                                                  2 * num_extension + 1 bits,
 *                                                  at the very top of the line
 *
 * A counter's low `stub_size` bits live in its stub. If it needs more, its
 * overflows bit is set and the remaining high bits are written into the
 * chunk's shared extension pool as a run of 2-bit fragments in base 3,
 * terminated by the fragment `11`. Extensions sit in the pool in the order of
 * the counters they belong to, so a counter's extension is found by ranking
 * its overflows bit and selecting the rank-th terminator in the pool.
 *
 * When a chunk's extensions no longer fit in its pool, the pool is replaced by
 * a 48-bit pointer to a heap-allocated *tails array* of one 32-bit tail per
 * counter, indexed by position rather than by rank. Too many chunks in that
 * state means the tuning of `(counters_per_chunk, stub_size)` -- VALE's
 * parameter pair -- no longer fits the data, and `MaybeRetune` re-derives it
 * from a histogram of the counter values and rebuilds the array.
 *
 * ---------------------------------------------------------------------------
 * Shifting
 * ---------------------------------------------------------------------------
 * A quotient filter moves slots around on nearly every insertion and deletion,
 * and the counters have to move with them. Doing that a counter at a time
 * would mean unpacking and repacking an extension per counter. It is not
 * necessary: a shift by one *preserves the relative order* of the counters it
 * moves, so within a chunk the extension pool -- which is ordered by exactly
 * that -- comes out bit-for-bit identical. All that has to move is the stubs
 * and the overflows bitmap, which are contiguous bit ranges and shift in a
 * handful of word operations.
 *
 * Only two things are left to do per chunk boundary the shift crosses: the one
 * counter that leaves the chunk has its extension removed from the pool it is
 * leaving, and re-inserted into the pool it arrives in. So the cost of moving
 * extensions is proportional to the number of chunks the shift spans, not to
 * the number of counters it moves.
 *
 * A tails array is the exception: it is indexed by position, not by rank, so
 * it has to be shifted along with the stubs. That is a `memmove`.
 *
 * ---------------------------------------------------------------------------
 * The range of a counter
 * ---------------------------------------------------------------------------
 * A tail is a `uint32_t`, as in `SublimeCMS`, so everything a counter carries
 * above its stub has to fit in 32 bits: a counter tops out at
 * `2^(32 + stub_size)`. Retuning lengthens the stub as the counts grow, and
 * with the shortest stub VALE will settle on that still leaves room for values
 * past 2^36. `MaxValue()` reports the current ceiling.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "util.hpp"

namespace sublime {

/**
 * A flat array of `counter_count` variable-length counters, tuned with VALE.
 *
 * Every counter starts at zero. The array never resizes itself; it is sized
 * once, to mirror a structure of a fixed slot count, and `MaybeRetune`
 * re-lays-out the same counters when their values outgrow the current tuning.
 */
class VALECounters {
    friend class VALECountersTest;
    friend class SublimeMGTest;

public:
    /* General cache line information. */
    static constexpr uint32_t cache_line_size = 512;
    static constexpr uint32_t cache_line_size_bytes = cache_line_size / 8;
    static constexpr uint32_t cache_line_size_words = cache_line_size_bytes / sizeof(uint64_t);

    // VALE's default counter per chunk size and the constraints enforced on it
    // during retuning. Identical to `SublimeCMS`.
    static constexpr uint32_t min_counter_per_cache_line = (cache_line_size / 512.0) * 16;
    static constexpr uint32_t max_counter_per_cache_line = (cache_line_size / 512.0) * 92;
    static constexpr uint32_t default_counter_per_cache_line =
            std::min<uint32_t>((cache_line_size / 512.0) * 68, (cache_line_size - 48) / 6);
    static_assert(min_counter_per_cache_line <= default_counter_per_cache_line
               && default_counter_per_cache_line <= max_counter_per_cache_line);
    // VALE's default stub size and the constraints enforced on it during retuning.
    static constexpr uint32_t min_stub_size = 4, max_stub_size = 32, default_stub_size = 5;
    static_assert(min_stub_size <= default_stub_size && default_stub_size <= max_stub_size);
    static_assert(default_counter_per_cache_line * (default_stub_size + 1) <= cache_line_size - 48);

private:
    /* VALE's extension fragment length. */
    static constexpr uint32_t extension_size = 2;
    static_assert((8 * sizeof(uint64_t)) % extension_size == 0,
                  "Word size must be divisible by the extension fragment size.");
    // VALE's constraints on the total extension fragment count within a chunk.
    static constexpr uint32_t min_extension_count = 24, max_extension_count = 64;
    // VALE's constraint on the probability of creating a tails array for a
    // chunk and the fraction of chunks allowed to have one before retuning.
    static constexpr float max_tail_probability = 0.03, retune_tail_frac = 0.03;
    // An upper bound on the number of extensions needed to encode a value of a
    // given bit length.
    static constexpr auto bit_length_to_extension_count = setup_extension_len_lookup_table();
    // Mask used to derive the `delimiters` bitmap in the rank-and-select workflow.
    static constexpr uint64_t select_mask = 0x5555555555555555;

    /**
     * The widest the extension pool can get, in words. Sized off
     * `max_extension_count` so that the pool can be unpacked onto the stack
     * without a variable-length array. The one word of slack lets the
     * carry-propagation loops read one fragment past the pool's last one.
     */
    static constexpr uint32_t max_extension_words = extension_size * max_extension_count / 64 + 2;

public:
    /**
     * Constructs an array of `counter_count` counters, all zero.
     *
     * @param with_min_tree Whether to lay a min segment tree over them, which
     * doubles the array: see the note at the top of this file. Without it the
     * array is exactly what it always was.
     */
    explicit VALECounters(uint64_t counter_count, bool with_min_tree = false) {
        // An even leaf count keeps every node's two children on the same side
        // of the leaf boundary -- both leaves or both internal -- which is
        // what lets the climb test a sibling without asking what kind of node
        // it is. An odd count gets one spare leaf, which stays empty.
        leaf_count_ = with_min_tree ? counter_count + (counter_count & 1) : 0;
        addressable_count_ = counter_count;
        allocate(with_min_tree ? 2 * leaf_count_ : counter_count, nullptr);
    }

    /**
     * Constructs an array of `counter_count` zero counters shaped the way
     * `like` is shaped. Used when a structure resizes: the counts about to be
     * copied over came out of an array with that tuning, so starting there
     * both skips an immediate retune and guarantees `MaxValue()` is wide
     * enough to hold every one of them.
     */
    VALECounters(uint64_t counter_count, const VALECounters& like) {
        leaf_count_ = like.HasMinTree() ? counter_count + (counter_count & 1) : 0;
        addressable_count_ = counter_count;
        allocate_with(like.HasMinTree() ? 2 * leaf_count_ : counter_count,
                      like.counters_per_chunk_, like.stub_size_);
    }

    ~VALECounters() {
        free_tails();
        delete[] chunks_;
    }

    VALECounters(const VALECounters& other) {
        leaf_count_ = other.leaf_count_;
        addressable_count_ = other.addressable_count_;
        min_pos_ = other.min_pos_;
        allocate_with(other.counter_count_, other.counters_per_chunk_, other.stub_size_);
        copy_values_from(other);
    }

    VALECounters(VALECounters&& other) noexcept {
        *this = std::move(other);
    }

    VALECounters& operator=(const VALECounters& other) {
        if (this == &other)
            return *this;
        // Copying the raw chunks would copy the tails array *pointers* along
        // with them, so a copy goes through the counter values instead.
        free_tails();
        delete[] chunks_;
        leaf_count_ = other.leaf_count_;
        addressable_count_ = other.addressable_count_;
        min_pos_ = other.min_pos_;
        allocate_with(other.counter_count_, other.counters_per_chunk_, other.stub_size_);
        copy_values_from(other);
        return *this;
    }

    VALECounters& operator=(VALECounters&& other) noexcept {
        if (this == &other)
            return *this;
        free_tails();
        delete[] chunks_;

        counter_count_ = other.counter_count_;
        leaf_count_ = other.leaf_count_;
        addressable_count_ = other.addressable_count_;
        min_pos_ = other.min_pos_;
        tree_suspended_ = other.tree_suspended_;
        chunk_count_ = other.chunk_count_;
        chunks_with_tails_ = other.chunks_with_tails_;
        tail_retune_limit_ = other.tail_retune_limit_;
        counters_per_chunk_ = other.counters_per_chunk_;
        stub_size_ = other.stub_size_;
        stub_mask_ = other.stub_mask_;
        num_extension_ = other.num_extension_;
        num_extension_words_ = other.num_extension_words_;
        last_extension_word_bit_count_ = other.last_extension_word_bit_count_;
        memcpy(word_update_byte_offset_, other.word_update_byte_offset_, sizeof(word_update_byte_offset_));
        memcpy(word_update_shamt_, other.word_update_shamt_, sizeof(word_update_shamt_));
        chunks_ = other.chunks_;

        other.chunks_ = nullptr;
        other.chunk_count_ = 0;
        other.counter_count_ = 0;
        other.chunks_with_tails_ = 0;
        return *this;
    }

    /** @returns The value of the counter at `pos`. */
    uint64_t Get(uint64_t pos) const {
        return get_raw(leaf_of(pos));
    }

    /** Sets the counter at `pos` to `value`, which must be at most `MaxValue()`. */
    void Set(uint64_t pos, uint64_t value) {
        const uint64_t at = leaf_of(pos);
        const uint64_t previous = has_live_tree() ? get_raw(at) : 0;
        set_raw(at, value);
        after_leaf_write(pos, previous, value);
    }

    /**
     * @returns The largest value a counter can hold under the current tuning.
     * Everything above the stub goes into a 32-bit tail, so the ceiling moves
     * with the stub length.
     */
    uint64_t MaxValue() const {
        return (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) << stub_size_) | stub_mask_;
    }

    /** Adds one to the counter at `pos`, carrying into its extension as needed. */
    void Increment(uint64_t pos) {
        const uint64_t at = leaf_of(pos);
        const uint64_t chunk = at / counters_per_chunk_;
        increment_counter(chunk, at - chunk * counters_per_chunk_);
        if (has_live_tree())
            after_leaf_increment(pos, get_raw(at));
    }

    /** Takes one off the counter at `pos`, borrowing from its extension as needed. */
    void Decrement(uint64_t pos) {
        const uint64_t at = leaf_of(pos);
        const uint64_t chunk = at / counters_per_chunk_;
        decrement_counter(chunk, at - chunk * counters_per_chunk_);
        if (has_live_tree()) {
            const uint64_t value = get_raw(at);
            after_leaf_write(pos, value + 1, value);
        }
    }

    /**
     * Takes one off the counter at `pos` and says whether that emptied it.
     *
     * The test costs nothing beyond the decrement itself: a counter holds zero
     * exactly when it has no extension and its stub reads zero, so neither the
     * extension pool nor a tails array has to be touched to find out -- which
     * is what makes a Misra-Gries decrement pass, which asks this of every
     * counter it touches, worth doing in one sweep.
     *
     * @returns True if the counter is now zero.
     */
    bool DecrementIsZero(uint64_t pos) {
        const uint64_t at = leaf_of(pos);
        const uint64_t chunk = at / counters_per_chunk_;
        const uint32_t inter_chunk = at - chunk * counters_per_chunk_;
        decrement_counter(chunk, inter_chunk);
        const bool empty = counter_is_zero(chunk, inter_chunk);
        assert(empty == (get_raw(at) == 0));
        if (has_live_tree()) {
            const uint64_t value = empty ? 0 : get_raw(at);
            after_leaf_write(pos, value + 1, value);
        }
        return empty;
    }

    /**
     * Brings the chunk holding the counter at `pos` into cache. Issued while
     * the caller is still walking the structure the counters mirror, so that
     * the counter is there by the time the walk lands on it.
     */
    __attribute__((always_inline))
    void Prefetch(uint64_t pos) const {
        __builtin_prefetch(chunks_ + (leaf_of(pos) / counters_per_chunk_) * cache_line_size_bytes);
    }

    /*
     * Prefetching a leaf's *ancestors* here, so that the climb's misses
     * overlap, was tried and is not worth it: it cost between 0 and 6% at
     * every depth from one ancestor to four, monotonically worse with each
     * extra one. There is nothing to hide. The internal nodes live in
     * `[1, n)`, which is 2 KB of counters for a 2160-slot table and 18 KB for
     * a 20000-slot one, so the climb stays inside L1 -- while the leaves it
     * starts from span the whole array. The leaf prefetch above, which
     * `FindMatch` issues before it touches the table at all, is the one that
     * pays.
     */

    /**
     * Opens a hole at `hole` by moving the counters in `[hole, last)` up one
     * position each, into `(hole, last]`. Whatever was at `last` is discarded,
     * and the counter at `hole` is left zero. Mirrors a quotient filter
     * shifting a run along to make room, where `last` is the empty slot the
     * shift consumes.
     */
    /*
     * Note: `ShiftRightAndClear`, `ShiftLeftAndClear` and the `repair_range`
     * behind them have **no caller in the tree**. They were the rank-and-select
     * quotient filter's, which slid a whole cluster along to open or close a
     * hole; the cuckoo filter that replaced it moves one counter at a time,
     * which the ordinary `Set` path handles. They are kept, and still tested,
     * as the array's own generality rather than any one table's.
     */

    void ShiftRightAndClear(uint64_t hole, uint64_t last) {
        assert(hole <= last && last < CountCounters());
        // The bulk shift writes over the far end of the range without reading
        // it, so anything there -- in particular an extension still sitting in
        // the pool -- has to be retired properly first.
        const bool candidate_discarded = has_live_tree() && min_pos_ == last;
        set_raw(leaf_of(last), 0);
        if (hole != last)
            move_range_up(leaf_of(hole), leaf_of(last) - 1);
        if (has_live_tree()) {
            repair_range(leaf_of(hole), leaf_of(last));
            // The candidate moved with its leaf; it did not change identity,
            // so there is nothing to search for.
            if (min_pos_ >= hole && min_pos_ < last)
                min_pos_++;
            else if (candidate_discarded)
                min_pos_ = descend_to_minimum();
        }
    }

    /**
     * Fills the hole at `hole` by moving the counters in `(hole, last]` down
     * one position each, into `[hole, last)`. Whatever was at `hole` is
     * discarded, and the counter at `last` is left zero. Mirrors a quotient
     * filter closing the gap a removal left behind.
     */
    void ShiftLeftAndClear(uint64_t hole, uint64_t last) {
        assert(hole <= last && last < CountCounters());
        const bool candidate_discarded = has_live_tree() && min_pos_ == hole;
        set_raw(leaf_of(hole), 0);
        if (hole != last)
            move_range_down(leaf_of(hole) + 1, leaf_of(last));
        if (has_live_tree()) {
            repair_range(leaf_of(hole), leaf_of(last));
            if (min_pos_ > hole && min_pos_ <= last)
                min_pos_--;
            // Unless the candidate was the counter this shift discarded, in
            // which case there is no choice but to look for another.
            else if (candidate_discarded)
                min_pos_ = descend_to_minimum();
        }
    }

    /** Zeroes every counter, keeping the current tuning. */
    void Reset() {
        free_tails();
        memset(chunks_, 0, static_cast<size_t>(chunk_count_) * cache_line_size_bytes);
        chunks_with_tails_ = 0;
        min_pos_ = 0;
    }

    /* The min segment tree. */

    /** @returns Whether a min segment tree is laid over the counters. */
    bool HasMinTree() const {
        return leaf_count_ != 0;
    }

    /**
     * @returns The smallest non-zero counter, or 0 if every one of them is
     * zero. The root holds it, shifted by one, so this costs one read.
     */
    uint64_t MinValue() const {
        assert(HasMinTree());
        const uint64_t root = get_raw(1);
        return root == 0 ? 0 : root - 1;
    }

    /** @returns A counter holding `MinValue()`. Meaningless if that is zero. */
    uint64_t MinSlot() const {
        assert(HasMinTree());
        return min_pos_;
    }

    /**
     * Stops maintaining the tree, for a caller about to write every counter
     * at once: one `RebuildTree` afterwards costs a single backward pass,
     * where a climb per write would cost `n log n`.
     */
    void SuspendTree() {
        tree_suspended_ = true;
    }

    /** Recomputes every internal node, and resumes maintenance. */
    void RebuildTree() {
        tree_suspended_ = false;
        if (!HasMinTree())
            return;

        // Each node's subtree minimum, outright, from the bottom up.
        for (uint64_t x = leaf_count_ - 1; x >= 1; x--) {
            set_raw(x, min_present(get_raw(2 * x), get_raw(2 * x + 1)));
            if (x == 1)
                break;
        }
        // Then down into differences. Going from the highest index to the
        // lowest converts a node's children while the node itself is still
        // outright: a node is only converted when its own parent is reached,
        // and a parent's index is always the smaller.
        for (uint64_t x = leaf_count_ - 1; x >= 1; x--) {
            const uint64_t m = get_raw(x);
            for (uint64_t child = 2 * x; child <= 2 * x + 1; child++)
                if (child < leaf_count_)                // Leaves keep their counts.
                    set_excess(child, get_raw(child), m);
            if (x == 1)
                break;
        }
        const uint64_t root = get_raw(1);               // The root is the minimum itself.
        set_raw(1, root == 0 ? 0 : root + 1);
        min_pos_ = descend_to_minimum();
    }

    /**
     * Re-derives VALE's parameters from the counters' current values and
     * rebuilds the array if too many chunks have spilled into tails arrays.
     *
     * @returns True if the array was rebuilt.
     */
    bool MaybeRetune() {
        return ShouldRetune() && Retune();
    }

    /** @returns True if enough chunks hold tails arrays to warrant a retune. */
    bool ShouldRetune() const {
        return chunks_with_tails_ >= tail_retune_limit_;
    }

    /**
     * Re-derives VALE's parameters and rebuilds the array under them, taking
     * `offset` off every non-zero counter on the way.
     *
     * A rebuild reads every counter out under one shape and writes it back
     * under another, so subtracting a constant from all of them as it goes
     * costs nothing beyond the pass that is happening anyway. A caller holding
     * a deferred decrement -- Misra-Gries' lazy decrement counter -- gets it
     * applied for free here rather than paying for a pass of its own, and the
     * new tuning is derived from what the counters read *after* the
     * subtraction, which is what they are about to hold.
     *
     * @param offset The amount to take off each non-zero counter. Every one of
     * them must be strictly greater than it -- in particular, no counter may
     * be left at zero, which a min tree would read as an empty slot.
     * @param shrank Whether the counters have got smaller since the current
     * tuning was chosen. It tells the tuning it is free to follow them
     * wherever they went, rather than only in the direction that relieves an
     * overflow -- which is the thrash the default rule guards against. A
     * non-zero `offset` implies it.
     * @returns True if the array was rebuilt. Counters that have not shrunk,
     * with no better tuning to move to, rebuild nothing and switch further
     * retuning off rather than retrying it on every subsequent update.
     */
    bool Retune(uint64_t offset = 0, bool shrank = false);

    /**
     * Retunes if the counters now call for a strictly narrower stub than the
     * array is using.
     *
     * The tails-fraction trigger behind `MaybeRetune` only fires when counters
     * grow past what their chunk can hold; nothing fires when they *shrink*,
     * which is what a Misra-Gries decrement pass does to all of them at once.
     * This is that missing direction, and requiring the stub to come back
     * strictly narrower is what keeps it from rebuilding for nothing.
     *
     * It costs a pass over the counters to work out, so a caller should ask
     * only when they can plausibly have shrunk by a whole bit -- see
     * `ShrinkRetuneInterval`.
     *
     * @returns True if the array was rebuilt.
     */
    bool RetuneIfNarrower();

    /**
     * @returns How many times every counter has to come down by one before a
     * stub one bit narrower could possibly fit them, i.e. how often asking
     * `RetuneIfNarrower` is worth the pass it costs.
     */
    uint64_t ShrinkRetuneInterval() const {
        return 1ULL << (stub_size_ - 1);
    }

    /* Size and shape. */

    /** @returns The number of counters a caller can address, i.e. the leaves. */
    uint64_t CountCounters() const {
        return HasMinTree() ? addressable_count_ : counter_count_;
    }
    /** @returns Every counter held, the tree's internal nodes included. */
    uint64_t CountStoredCounters() const {
        return counter_count_;
    }
    uint64_t CountChunks() const {
        return chunk_count_;
    }
    uint32_t GetCountersPerChunk() const {
        return counters_per_chunk_;
    }
    uint32_t GetStubLength() const {
        return stub_size_;
    }
    uint32_t CountChunksWithTails() const {
        return chunks_with_tails_;
    }
    /** @returns The bytes held, the heap-allocated tails arrays included. */
    uint64_t SizeInBytes() const {
        return static_cast<uint64_t>(chunk_count_) * cache_line_size_bytes
             + static_cast<uint64_t>(chunks_with_tails_) * counters_per_chunk_ * sizeof(uint32_t);
    }

private:
    /** Rebuilds `other`'s counters, less `offset`, under the given tuning. */
    VALECounters(const VALECounters& other, uint32_t counters_per_chunk, uint32_t stub_size,
                 uint64_t offset = 0) {
        leaf_count_ = other.leaf_count_;
        addressable_count_ = other.addressable_count_;
        min_pos_ = other.min_pos_;
        allocate_with(other.counter_count_, counters_per_chunk, stub_size);
        copy_values_from(other, offset);
    }

    /**
     * Reads every counter of `other` out under its shape and writes it under
     * ours, less `offset`. A counter of zero stands for one nothing is using,
     * and stays zero rather than going negative.
     *
     * This copies raw indices, so a min tree comes across whole. The offset
     * reaches the leaves and the root, which hold counts; the nodes between
     * them hold differences between minima, which a uniform offset leaves
     * alone. Nothing has to be recomputed.
     */
    void copy_values_from(const VALECounters& other, uint64_t offset = 0) {
        assert(counter_count_ == other.counter_count_);
        for (uint64_t i = 0; i < counter_count_; i++) {
            const uint64_t value = other.get_raw(i);
            if (value == 0)
                continue;
            // With a min tree, only the leaves and the root hold counts. Every
            // other node holds a *difference* between two minima, and taking
            // the same amount off both of them leaves it exactly as it was.
            if (offset != 0 && HasMinTree() && i != 1 && i < leaf_count_) {
                set_raw(i, value);
                continue;
            }
            assert(value > offset);
            set_raw(i, value - offset);
        }
    }

    uint64_t counter_count_ = 0;
    uint32_t chunk_count_ = 0;
    uint32_t chunks_with_tails_ = 0;
    uint32_t tail_retune_limit_ = 0;
    /**
     * The number of leaves, if a min segment tree is laid over the counters,
     * and zero if not. With one, `counter_count_` is `2 * leaf_count_`: the
     * leaves sit in `[leaf_count_, 2 * leaf_count_)` and the internal nodes in
     * `[1, leaf_count_)`, with node `i`'s children at `2i` and `2i + 1`. Index
     * zero goes unused. That is the bottom-up segment tree layout, whose root
     * aggregates every leaf whether or not the count is a power of two -- and
     * it never is here, since it is a quotient filter's slot capacity.
     */
    uint64_t leaf_count_ = 0;
    /** How many of those leaves the caller may address; see the constructor. */
    uint64_t addressable_count_ = 0;
    /** A leaf holding `MinValue()`, tracked as the counters move. */
    uint64_t min_pos_ = 0;
    /** Set while a caller is writing every counter; see `SuspendTree`. */
    bool tree_suspended_ = false;

    /** Adds one to the counter at a raw index, tree nodes included. */
    void increment_raw(uint64_t pos) {
        const uint64_t chunk = pos / counters_per_chunk_;
        increment_counter(chunk, pos - chunk * counters_per_chunk_);
    }
    /** Takes one off the counter at a raw index, tree nodes included. */
    void decrement_raw(uint64_t pos) {
        const uint64_t chunk = pos / counters_per_chunk_;
        decrement_counter(chunk, pos - chunk * counters_per_chunk_);
    }

    /** Reads the counter at a raw index, tree nodes included. */
    uint64_t get_raw(uint64_t pos) const;
    /** Writes the counter at a raw index, tree nodes included. */
    void set_raw(uint64_t pos, uint64_t value);

    /** @returns The counter index a caller's `pos` actually lives at. */
    uint64_t leaf_of(uint64_t pos) const {
        return leaf_count_ + pos;       // `leaf_count_` is zero without a tree.
    }

    bool has_live_tree() const {
        return HasMinTree() && !tree_suspended_;
    }

    /**
     * The minimum of two subtree minima, where zero means *no entry here*
     * rather than a count of zero. A monitored key's counter is always at
     * least one, so zero is unambiguous, and it leaves an empty slot costing
     * a zero stub and nothing else.
     */
    static uint64_t min_present(uint64_t a, uint64_t b) {
        if (a == 0)
            return b;
        if (b == 0)
            return a;
        return a < b ? a : b;
    }

    /** True if the counter at a raw index holds exactly one. */
    bool raw_is_one(uint64_t pos) const {
        const uint64_t chunk = pos / counters_per_chunk_;
        const uint32_t inter_chunk = pos - chunk * counters_per_chunk_;
        const uint8_t *chunk_ptr_ = chunk_ptr(chunk);
        const uint64_t *words = reinterpret_cast<const uint64_t *>(chunk_ptr_);
        if (is_overflowing(words, inter_chunk))
            return false;
        const uint64_t *read_word = reinterpret_cast<const uint64_t *>(chunk_ptr_
                                                        + word_update_byte_offset_[inter_chunk]);
        return ((read_word[0] >> word_update_shamt_[inter_chunk]) & stub_mask_) == 1;
    }

    /** True if the counter at a raw index holds zero. */
    bool raw_is_zero(uint64_t pos) const {
        const uint64_t chunk = pos / counters_per_chunk_;
        return counter_is_zero(chunk, pos - chunk * counters_per_chunk_);
    }

    /** Writes `node`'s difference against its parent's minimum. */
    void set_excess(uint64_t node, uint64_t node_minimum, uint64_t parent_minimum) {
        assert(node_minimum == 0 || node_minimum >= parent_minimum);
        set_raw(node, node_minimum == 0 ? 0 : node_minimum - parent_minimum + 1);
    }

    /**
     * Walks from the root to a leaf holding the smallest counter, following a
     * child that sits at its parent's minimum -- an internal one reading 1, or,
     * at the bottom, the leaf whose count is the minimum itself.
     *
     * @returns That leaf as a caller-facing position, or 0 if nothing is
     * stored at all.
     */
    uint64_t descend_to_minimum() const {
        const uint64_t root = get_raw(1);
        if (root == 0)
            return 0;
        const uint64_t smallest = root - 1;
        uint64_t x = 1;
        while (x < leaf_count_) {
            const uint64_t left = 2 * x;
            x = (left >= leaf_count_ ? (get_raw(left) == smallest ? left : left + 1)
                                     : (raw_is_one(left) ? left : left + 1));
        }
        return x - leaf_count_;
    }

    /**
     * The minimum of every node from the root down to `node`, written into
     * `minima` root-first, with the nodes themselves in `nodes`.
     *
     * @returns How many levels that is.
     */
    uint32_t path_to(uint64_t node, uint64_t *nodes, uint64_t *minima) const {
        uint32_t depth = 0;
        for (uint64_t x = node; x >= 1; x >>= 1)
            nodes[depth++] = x;
        for (uint32_t i = 0; i < depth / 2; i++)
            std::swap(nodes[i], nodes[depth - 1 - i]);

        const uint64_t root = get_raw(1);
        minima[0] = root == 0 ? 0 : root - 1;
        for (uint32_t i = 1; i < depth; i++) {
            const uint64_t excess = get_raw(nodes[i]);
            minima[i] = excess == 0 ? 0 : minima[i - 1] + excess - 1;
        }
        return depth;
    }

    /**
     * The climb an increment takes, given that `x`'s subtree minimum has just
     * risen by one. Nothing here reads a counter's value: `raw_is_one` and
     * `raw_is_zero` answer from the overflow bit and the stub, and the updates
     * are increments and decrements. That is the whole point of holding
     * differences rather than minima.
     */
    void minimum_rose_by_one(uint64_t x) {
        while (true) {
            if (x == 1) {                       // The root holds the minimum itself.
                increment_raw(1);
                return;
            }
            if (!raw_is_one(x)) {
                // `x` was not sitting at its parent's minimum, so the parent's
                // does not move; `x` is simply that much further above it.
                increment_raw(x);
                return;
            }
            const uint64_t sibling = x ^ 1;
            if (raw_is_one(sibling)) {
                increment_raw(x);               // The sibling still holds it.
                return;
            }
            // Nothing else holds the parent's minimum, so it rises too -- and
            // every child's difference against it falls by one, which leaves
            // `x` exactly where it was and takes one off the sibling.
            if (!raw_is_zero(sibling))
                decrement_raw(sibling);
            x >>= 1;
        }
    }

    /**
     * The general update, for a leaf that has just been written with a value
     * that may move its subtree's minimum anywhere: walk down for the minima
     * the differences are relative to, then back up rewriting them, stopping
     * at the first node whose minimum did not move.
     */
    void leaf_written(uint64_t pos) {
        const uint64_t leaf = leaf_of(pos);
        uint64_t nodes[2 * sizeof(uint64_t) * 8], minima[2 * sizeof(uint64_t) * 8];
        const uint32_t depth = path_to(leaf >> 1, nodes, minima);

        // The bottom level is the one place both children are leaves, so its
        // minimum is read off them directly.
        uint64_t node_minimum = min_present(get_raw(leaf), get_raw(leaf ^ 1));
        for (int32_t i = static_cast<int32_t>(depth) - 1; ; i--) {
            if (i == 0) {
                set_raw(1, node_minimum == 0 ? 0 : node_minimum + 1);
                return;
            }
            if (node_minimum == minima[i])
                return;                         // Unmoved, so nothing above it moves.

            const uint64_t x = nodes[i], sibling = x ^ 1;
            const uint64_t sibling_excess = get_raw(sibling);
            const uint64_t sibling_minimum = sibling_excess == 0
                                                ? 0 : minima[i - 1] + sibling_excess - 1;
            const uint64_t parent_minimum = min_present(node_minimum, sibling_minimum);
            // Both children are relative to the parent, so both are rewritten.
            set_excess(x, node_minimum, parent_minimum);
            set_excess(sibling, sibling_minimum, parent_minimum);
            node_minimum = parent_minimum;
        }
    }

    /**
     * The general update, told what the leaf held before. Knowing that, the
     * minima the differences are relative to can be *derived* on the way up --
     * a node's parent's minimum is its own less its stored difference -- so
     * the walk down `leaf_written` makes is not needed, and a write that does
     * not move its pair's minimum costs two reads and nothing else.
     *
     * The one case that cannot be derived is a node that was empty, whose
     * parent's minimum came from elsewhere entirely; that falls back.
     */
    void leaf_changed(uint64_t pos, uint64_t old_value, uint64_t new_value) {
        const uint64_t leaf = leaf_of(pos);
        const uint64_t sibling = get_raw(leaf ^ 1);
        uint64_t x = leaf >> 1;
        uint64_t old_minimum = min_present(old_value, sibling);
        uint64_t new_minimum = min_present(new_value, sibling);

        while (old_minimum != new_minimum) {
            if (x == 1) {
                set_raw(1, new_minimum == 0 ? 0 : new_minimum + 1);
                return;
            }
            const uint64_t excess = get_raw(x);
            if (excess == 0) {              // Was empty: nothing to derive from.
                leaf_written(pos);
                return;
            }
            const uint64_t parent_old = old_minimum - (excess - 1);
            const uint64_t sib = x ^ 1;
            const uint64_t sib_excess = get_raw(sib);
            const uint64_t sib_minimum = sib_excess == 0 ? 0 : parent_old + sib_excess - 1;
            const uint64_t parent_new = min_present(new_minimum, sib_minimum);
            set_excess(x, new_minimum, parent_new);
            set_excess(sib, sib_minimum, parent_new);
            old_minimum = parent_old;
            new_minimum = parent_new;
            x >>= 1;
        }
    }

    /**
     * Maintains the tree and the candidate after the leaf at `pos` was written
     * with `value`, where it held `old_value`.
     */
    void after_leaf_write(uint64_t pos, uint64_t old_value, uint64_t value) {
        if (!has_live_tree())
            return;
        leaf_changed(pos, old_value, value);
        note_candidate(pos, value);
    }

    /** The cheap path: the leaf at `pos` was incremented to `value`. */
    void after_leaf_increment(uint64_t pos, uint64_t value) {
        if (!has_live_tree())
            return;
        if (value <= 1) {
            // The slot was empty, so this is a new entry rather than a count
            // going up, and it may pull a minimum *down*. That is the general
            // update's business.
            leaf_written(pos);
            note_candidate(pos, value);
            return;
        }
        const uint64_t leaf = leaf_of(pos);
        const uint64_t sibling = get_raw(leaf ^ 1);
        // The pair's minimum rises only if this leaf was the whole of it.
        if (sibling == 0 || value <= sibling)
            minimum_rose_by_one(leaf >> 1);
        note_candidate(pos, value);
    }

    /** Keeps `min_pos_` on a leaf that holds the minimum. */
    void note_candidate(uint64_t pos, uint64_t value) {
        if (value != 0 && value == MinValue())
            min_pos_ = pos;
        else if (pos == min_pos_)
            min_pos_ = descend_to_minimum();
    }

    /**
     * Repairs the tree after a shift moved every leaf in `[lo, hi]`, given
     * what those leaves held before it. Shifting is the RSQF's business -- the
     * cuckoo table moves one entry at a time -- so this is a loop of the
     * general update rather than anything cleverer. It has to be the walking
     * form: the derived form reads the tree to work out what a minimum *was*,
     * and after the first leaf of the range has been applied that reading is
     * no longer of the state the rest of the range was measured against.
     */
    void repair_range(uint64_t lo, uint64_t hi) {
        for (uint64_t leaf = lo; leaf <= hi; leaf++)
            leaf_written(leaf - leaf_count_);
    }

    uint32_t counters_per_chunk_ = 0;
    uint32_t stub_size_ = 0;
    uint64_t stub_mask_ = 0;
    uint32_t num_extension_ = 0;                /**< Fragments that fit in a chunk's extension pool. */
    uint32_t num_extension_words_ = 0;          /**< Words the pool is unpacked into. */
    uint32_t last_extension_word_bit_count_ = 0;/**< Bits of the pool held in its last word. */
    /* For each counter of a chunk, the word its stub starts in and its offset there. */
    uint8_t word_update_byte_offset_[max_counter_per_cache_line] = {};
    uint8_t word_update_shamt_[max_counter_per_cache_line] = {};
    uint8_t *chunks_ = nullptr;

    uint8_t *chunk_ptr(uint64_t chunk) const {
        return chunks_ + chunk * cache_line_size_bytes;
    }
    uint64_t *chunk_words(uint64_t chunk) const {
        return reinterpret_cast<uint64_t *>(chunk_ptr(chunk));
    }

    /*
     * ------------------------------------------------------------------
     * The bit fiddling behind the layout. Adapted from `SublimeCMS`.
     * ------------------------------------------------------------------
     */

    /**
     * Computes, for every counter of a chunk, which word its stub starts in
     * and its offset within that word. Stubs are at most 32 bits, so aligning
     * a stub that straddles a 64-bit boundary to the enclosing 32-bit half
     * keeps every read and write to a single word.
     */
    void setup_lookup_tables();

    /**
     * @returns How many extension fragments a chunk's pool holds under the
     * tuning `(counters_per_chunk, stub_size)`. The pool takes whatever the
     * stubs leave, up to VALE's cap: past that it is only wasting the cache
     * line, since no chunk's extensions could use the room.
     */
    static uint32_t extension_count_for(uint32_t counters_per_chunk, uint32_t stub_size) {
        return std::min<uint32_t>(
                (cache_line_size - 1 - counters_per_chunk * (stub_size + 1)) / extension_size,
                max_extension_count);
    }

    /** Recomputes everything derived from `(counters_per_chunk_, stub_size_)`. */
    void reshape() {
        stub_mask_ = BITMASK(stub_size_);
        num_extension_ = extension_count_for(counters_per_chunk_, stub_size_);
        num_extension_words_ = extension_size * num_extension_ / 64 + 1;
        last_extension_word_bit_count_ = extension_size * num_extension_ + 1 - 64 * (num_extension_words_ - 1);
        assert(num_extension_words_ + 1 <= max_extension_words);
        setup_lookup_tables();
    }

    __attribute__((always_inline))
    void set_overflowing(uint64_t words[], uint32_t pos, bool state) const {
        words[pos / 64] = state ? words[pos / 64] | (1ULL << (pos % 64))
                                : words[pos / 64] & ~(1ULL << (pos % 64));
    }
    __attribute__((always_inline))
    void set_overflowing_to_0(uint64_t words[], uint32_t pos, uint64_t bit_value = 1) const {
        words[pos / 64] &= ~(bit_value << (pos % 64));
    }
    __attribute__((always_inline))
    void set_overflowing_to_1(uint64_t words[], uint32_t pos, uint64_t bit_value = 1) const {
        words[pos / 64] |= bit_value << (pos % 64);
    }
    __attribute__((always_inline))
    /**
     * @returns Whether the counter at `inter_chunk` of `chunk` holds zero,
     * read from its overflow bit and its stub alone.
     */
    bool counter_is_zero(uint64_t chunk, uint32_t inter_chunk) const {
        const uint8_t *chunk_ptr_ = chunk_ptr(chunk);
        const uint64_t *words = reinterpret_cast<const uint64_t *>(chunk_ptr_);
        if (is_overflowing(words, inter_chunk))
            return false;
        const uint64_t *read_word = reinterpret_cast<const uint64_t *>(chunk_ptr_
                                                        + word_update_byte_offset_[inter_chunk]);
        return ((read_word[0] >> word_update_shamt_[inter_chunk]) & stub_mask_) == 0;
    }

    bool is_overflowing(const uint64_t words[], uint32_t pos) const {
        return (words[pos / 64] >> (pos % 64)) & 1ULL;
    }
    __attribute__((always_inline))
    uint64_t get_delimiters_bitmap_word(uint64_t val) const {
        return (val & (val >> 1)) & select_mask;
    }

    /** @returns The rank of a chunk's counter among its overflowing counters. */
    uint32_t get_extension_rank(const uint64_t words[], uint32_t pos) const {
        uint32_t res = 0;
        for (uint32_t i = 0; i < pos / 64; i++)
            res += __builtin_popcountll(words[i]);
        return res + bit_rank(words[pos / 64], pos % 64);
    }

    /** @returns The bit position within the pool of the extension of rank `rank`. */
    uint32_t get_extension_pos(const uint64_t extensions[], uint32_t rank) const;

    /** @returns The total length of a chunk's extensions, in bits. */
    uint32_t get_extension_length(const uint64_t extensions[]) const;

    /** Shifts the pool from bit `pos` up by `shamt` bits, zeroing what it vacates. */
    void shift_extensions_left_from_pos(uint64_t extensions[], uint32_t pos, uint32_t shamt) const;

    /** Shifts the pool from bit `pos` down by `shamt` bits, dropping what it overwrites. */
    void shift_extensions_right_from_pos(uint64_t extensions[], uint32_t pos, uint32_t shamt) const;

    /** Unpacks a chunk's extension pool out of the top of its cache line. */
    __attribute__((always_inline))
    void read_extensions(const uint64_t *words, uint64_t extensions[]) const {
        for (uint32_t i = 0; i < num_extension_words_ - 1; i++)
            extensions[i] = words[cache_line_size_words - i - 1];
        extensions[num_extension_words_ - 1] = words[cache_line_size_words - num_extension_words_]
                                                    >> (64 - last_extension_word_bit_count_);
        extensions[num_extension_words_] = 0;   // Slack, so a carry may read one fragment past the end.
    }

    /** Packs a chunk's extension pool back into the top of its cache line. */
    void write_extensions(uint64_t *words, uint64_t extensions[]) const {
        extensions[num_extension_words_ - 1] =
                (extensions[num_extension_words_ - 1] << (64 - last_extension_word_bit_count_))
                | (words[cache_line_size_words - num_extension_words_]
                        & BITMASK(64 - last_extension_word_bit_count_));
        for (uint32_t i = 0; i < num_extension_words_ - 1; i++)
            words[cache_line_size_words - i - 1] = extensions[i];
        words[cache_line_size_words - num_extension_words_] = extensions[num_extension_words_ - 1];
    }

    __attribute__((always_inline))
    bool has_tails_array(const uint64_t *words) const {
        return words[cache_line_size_words - num_extension_words_] >> 63;
    }

    uint32_t *get_tails_ptr(const uint64_t *extensions) const {
        // The low 48 bits of the pointer are enough to rebuild it on x86-64.
        return reinterpret_cast<uint32_t *>(extensions[0] & BITMASK(extension_size * min_extension_count));
    }

    /** Moves a chunk's extensions out of its pool and into a fresh tails array. */
    uint32_t *setup_tails_array(const uint64_t *overflows_bitmap, uint64_t *extensions,
                                uint32_t total_extension_len);

    void increment_counter(uint64_t chunk, uint32_t inter_chunk);
    void decrement_counter(uint64_t chunk, uint32_t inter_chunk);

    /*
     * ------------------------------------------------------------------
     * Bulk bit moves, the primitives the shifting paths are built on.
     * ------------------------------------------------------------------
     */

    /** @returns The `len` bits of the chunk starting at bit `pos`. `len <= 64`. */
    __attribute__((always_inline))
    static uint64_t read_chunk_bits(const uint64_t *words, uint32_t pos, uint32_t len) {
        const uint32_t word = pos / 64, offset = pos % 64;
        uint64_t res = words[word] >> offset;
        if (offset && word + 1 < cache_line_size_words)
            res |= words[word + 1] << (64 - offset);
        return res & BITMASK(len);
    }

    /** Writes `len` bits of `value` into the chunk at bit `pos`. `len <= 64`. */
    __attribute__((always_inline))
    static void write_chunk_bits(uint64_t *words, uint32_t pos, uint32_t len, uint64_t value) {
        const uint32_t word = pos / 64, offset = pos % 64;
        const uint64_t mask = BITMASK(len);
        value &= mask;
        words[word] = (words[word] & ~(mask << offset)) | (value << offset);
        if (offset + len > 64) {
            const uint32_t rest = offset + len - 64;
            words[word + 1] = (words[word + 1] & ~BITMASK(rest)) | (value >> (64 - offset));
        }
    }

    /**
     * Moves the bits of a chunk in `[start, end)` up by `shamt`, zeroing the
     * `shamt` bits they vacate at `start`. Everything at or above `end + shamt`
     * is left alone -- which is what keeps the extension pool, sitting just
     * above the stubs, out of the way.
     */
    static void shift_chunk_bits_up(uint64_t *words, uint32_t start, uint32_t end, uint32_t shamt);

    /** The mirror image of `shift_chunk_bits_up`, moving `[start, end)` down. */
    static void shift_chunk_bits_down(uint64_t *words, uint32_t start, uint32_t end, uint32_t shamt);

    /**
     * Moves the counters at the chunk-local positions `[lo, hi]` up one
     * position, leaving `lo` zero. Touches only the stubs, the overflows
     * bitmap, and the tails array if there is one: the extension pool orders
     * its entries by the position of the counters they belong to, and this
     * move leaves that order alone.
     */
    void shift_within_chunk_up(uint64_t chunk, uint32_t lo, uint32_t hi);

    /** The mirror image of `shift_within_chunk_up`, moving `[lo, hi]` down. */
    void shift_within_chunk_down(uint64_t chunk, uint32_t lo, uint32_t hi);

    /** Moves the counters in `[first, last]` up one position, leaving `first` zero. */
    void move_range_up(uint64_t first, uint64_t last);

    /** Moves the counters in `[first, last]` down one position, leaving `last` zero. */
    void move_range_down(uint64_t first, uint64_t last);

    /*
     * ------------------------------------------------------------------
     * Tuning.
     * ------------------------------------------------------------------
     */

    /**
     * Picks `(counters_per_chunk, stub_size)` for a given distribution of
     * counter values. Identical to `SublimeCMS::tune_params`: it walks the
     * candidate pairs from the most counters per chunk down, and takes the
     * first whose extension pool is big enough that, by Chebyshev's
     * inequality, a chunk overflows into a tails array with probability at
     * most `max_tail_probability`.
     *
     * @param counter_len_cnt A histogram of the counters by value length, in
     * bits, or null for the default tuning.
     */
    static std::pair<uint32_t, uint32_t> tune_params(const uint32_t *counter_len_cnt);

    /**
     * A histogram of the counters by the bit length of their values. Needs
     * `length_histogram_size` entries.
     */
    static constexpr uint32_t length_histogram_size = 8 * sizeof(uint64_t) + 1;
    void compute_counter_len_cnt(uint32_t *counter_len_cnt, uint64_t offset = 0) const {
        for (uint64_t i = 0; i < counter_count_; i++) {
            const uint64_t raw = get_raw(i);
            const uint64_t value = raw > offset ? raw - offset : 0;
            counter_len_cnt[value ? highbit_pos(value) + 1 : 0]++;
        }
    }

    /**
     * Sizes and zeroes the array for `counter_count` counters under the tuning
     * `counter_len_cnt` calls for.
     *
     */
    void allocate(uint64_t counter_count, const uint32_t *counter_len_cnt) {
        auto [counters_per_chunk, stub_size] = tune_params(counter_len_cnt);
        allocate_with(counter_count, counters_per_chunk, stub_size);
    }

    /** Sizes and zeroes the array under an explicit tuning. */
    void allocate_with(uint64_t counter_count, uint32_t counters_per_chunk, uint32_t stub_size);

    void free_tails();
};


/******************************************************************
 * Shape and allocation.                                          *
 ******************************************************************/

inline void VALECounters::setup_lookup_tables() {
    constexpr uint64_t word_size_bytes = sizeof(uint64_t);
    constexpr uint64_t word_size_bits = word_size_bytes * 8;
    for (uint32_t i = 0; i < counters_per_chunk_; i++) {
        const uint32_t counter_pos = i * stub_size_ + counters_per_chunk_;
        if ((counter_pos % word_size_bits) + stub_size_ > word_size_bits) {
            // Couldn't use a 64-bit address for the word.
            word_update_byte_offset_[i] = (counter_pos / word_size_bits) * word_size_bytes
                                            + word_size_bytes / 2;
            word_update_shamt_[i] = counter_pos % word_size_bits - word_size_bits / 2;
        }
        else {
            // Managed to use a 64-bit address for the word.
            word_update_byte_offset_[i] = counter_pos / word_size_bits * word_size_bytes;
            word_update_shamt_[i] = counter_pos % word_size_bits;
        }
    }
}


inline std::pair<uint32_t, uint32_t> VALECounters::tune_params(const uint32_t *counter_len_cnt) {
    if (counter_len_cnt == nullptr)     // Default case.
        return {default_counter_per_cache_line, default_stub_size};

    const uint32_t word_size_bits = 8 * sizeof(uint64_t);
    // Partial sums of the histogram, i.e. how many counters a given stub size
    // holds outright.
    uint64_t counter_len_ps[word_size_bits] = {counter_len_cnt[0]};
    for (uint32_t i = 1; i < word_size_bits; i++)
        counter_len_ps[i] = counter_len_ps[i - 1] + counter_len_cnt[i];

    for (int32_t m_c = max_counter_per_cache_line; m_c >= static_cast<int32_t>(min_counter_per_cache_line); m_c--) {
        const uint32_t cache_line_cnt = std::max<uint64_t>(1, counter_len_ps[word_size_bits - 1] / m_c);
        for (uint32_t m_s = min_stub_size; m_s <= max_stub_size; m_s++) {
            const int32_t extension_bitmap_len = cache_line_size - m_c * (m_s + 1) - 1;
            // Enforce a minimum extension pool size, so that it can hold a pointer.
            if (extension_bitmap_len > static_cast<int32_t>(extension_size * max_extension_count))
                continue;
            // Enforce a maximum extension pool size, for efficiency.
            if (extension_bitmap_len < static_cast<int32_t>(extension_size * min_extension_count))
                break;
            float expected_extension_len = 0, var_extension_len = 0;
            for (uint32_t i = m_s + 1; i < word_size_bits; i++) {
                const uint64_t term = extension_size * bit_length_to_extension_count[i - m_s];
                const float expected_term = static_cast<float>(term) / cache_line_cnt;
                const float var_term = static_cast<float>(term * term) / cache_line_cnt
                                        - expected_term * expected_term;
                expected_extension_len += expected_term * counter_len_cnt[i];
                var_extension_len += var_term * counter_len_cnt[i];
            }
            const float deviation = extension_bitmap_len - expected_extension_len;
            if (deviation < 0)
                continue;
            if (var_extension_len / (deviation * deviation) > max_tail_probability)  // Chebyshev's inequality.
                continue;
            return {m_c, m_s};
        }
    }
    return {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
}


inline void VALECounters::allocate_with(uint64_t counter_count, uint32_t counters_per_chunk,
                                        uint32_t stub_size) {
    assert(min_counter_per_cache_line <= counters_per_chunk
        && counters_per_chunk <= max_counter_per_cache_line);
    assert(min_stub_size <= stub_size && stub_size <= max_stub_size);
    // The pool has to stay wide enough to hold a tails array pointer.
    assert(counters_per_chunk * (stub_size + 1)
            <= cache_line_size - 1 - extension_size * min_extension_count);

    counter_count_ = counter_count;
    counters_per_chunk_ = counters_per_chunk;
    stub_size_ = stub_size;
    chunk_count_ = (counter_count + counters_per_chunk - 1) / counters_per_chunk;
    tail_retune_limit_ = chunk_count_ * retune_tail_frac + 1;
    chunks_with_tails_ = 0;
    reshape();

    chunks_ = new uint8_t[static_cast<size_t>(chunk_count_) * cache_line_size_bytes]{};
}


inline void VALECounters::free_tails() {
    if (chunks_ == nullptr || chunks_with_tails_ == 0)
        return;
    uint64_t extensions[max_extension_words];
    for (uint64_t c = 0; c < chunk_count_; c++) {
        uint64_t *words = chunk_words(c);
        if (!has_tails_array(words))
            continue;
        read_extensions(words, extensions);
        delete[] get_tails_ptr(extensions);
    }
    chunks_with_tails_ = 0;
}


inline bool VALECounters::Retune(uint64_t offset, bool shrank) {
    shrank |= (offset != 0);
    uint32_t counter_len_cnt[length_histogram_size] = {};
    compute_counter_len_cnt(counter_len_cnt, offset);
    auto [counters_per_chunk, stub_size] = tune_params(counter_len_cnt);
    if (counters_per_chunk > max_counter_per_cache_line) {
        // No candidate pair met the tail-probability bound, so fall back.
        counters_per_chunk = default_counter_per_cache_line;
        stub_size = default_stub_size;
    }
    // If the stub length comes back unchanged, the counters per chunk has to
    // strictly decrease, so that a retune cannot land back on the very
    // configuration that just overflowed and go on triggering itself. That
    // only applies when nothing else about the counters changed: counters that
    // genuinely shrank let the tuning follow them wherever they went, upwards
    // included.
    if (!shrank && stub_size == stub_size_)
        counters_per_chunk = std::min(counters_per_chunk, counters_per_chunk_ - 1);
    counters_per_chunk = std::max(counters_per_chunk, min_counter_per_cache_line);

    // Stop when there is nothing left to win. Keeping the same stub length and
    // taking counters out of the chunk only pays while it buys a wider
    // extension pool; once the pool is capped, all it buys is more chunks.
    if ((counters_per_chunk == counters_per_chunk_ && stub_size == stub_size_)
            || (stub_size == stub_size_
                && extension_count_for(counters_per_chunk, stub_size) <= num_extension_)) {
        if (!shrank) {
            tail_retune_limit_ = chunk_count_ + 1;
            return false;
        }
        // The offset still has to come off, and this is the pass that does
        // it. Counters the caller already shrank are rebuilt under the same
        // shape too, which re-derives the tail bookkeeping against the values
        // they now hold.
        counters_per_chunk = counters_per_chunk_;
        stub_size = stub_size_;
    }

    // The two shapes disagree about where everything lives, so the rebuild
    // reads each counter out under the old one and writes it back under the new.
    VALECounters rebuilt(*this, counters_per_chunk, stub_size, offset);
    *this = std::move(rebuilt);
    return true;
}


inline bool VALECounters::RetuneIfNarrower() {
    uint32_t counter_len_cnt[length_histogram_size] = {};
    compute_counter_len_cnt(counter_len_cnt);
    auto [counters_per_chunk, stub_size] = tune_params(counter_len_cnt);
    // No candidate pair met the tail-probability bound, or the counters have
    // not shrunk enough to pay for a rebuild.
    if (counters_per_chunk > max_counter_per_cache_line || stub_size >= stub_size_)
        return false;

    // The rebuilt array derives its own tail bookkeeping, so this also
    // switches retuning back on if the old shape had given up on it.
    VALECounters rebuilt(*this, counters_per_chunk, stub_size);
    *this = std::move(rebuilt);
    return true;
}


/******************************************************************
 * Extension pool primitives, adapted from `SublimeCMS`.          *
 ******************************************************************/

__attribute__((always_inline))
inline uint32_t VALECounters::get_extension_pos(const uint64_t extensions[], uint32_t rank) const {
    uint64_t delimiters[max_extension_words];
    for (uint32_t i = 0; i < num_extension_words_; i++)
        delimiters[i] = get_delimiters_bitmap_word(extensions[i]);
    int extension_pos = (rank == 0 ? -2 : static_cast<int>(bit_select(delimiters[0], rank - 1)));
    if (num_extension_words_ == 2) {
        const int32_t other_attempt = bit_select(delimiters[1], rank - 1 - __builtin_popcountll(delimiters[0]));
        extension_pos = (extension_pos >= 64 ? other_attempt + 64 : extension_pos);
    }
    else if (num_extension_words_ > 2) {
        extension_pos = (extension_pos < 64 ? extension_pos : -3);
        uint32_t running_rank = rank - __builtin_popcountll(delimiters[0]);
        for (uint32_t i = 1; i < num_extension_words_; i++) {
            const uint32_t select_result = running_rank > 64 ? 64 : bit_select(delimiters[i], running_rank - 1);
            extension_pos = ((extension_pos == -3 && select_result < 64) ? select_result + i * 64 : extension_pos);
            running_rank -= __builtin_popcountll(delimiters[i]);
        }
    }
    return extension_pos + 2;
}


__attribute__((always_inline))
inline uint32_t VALECounters::get_extension_length(const uint64_t extensions[]) const {
    if (num_extension_words_ == 1)
        return highbit_pos(extensions[0]) + 1;
    else if (num_extension_words_ == 2) {
        const uint32_t a = highbit_pos(extensions[1]);
        const uint32_t b = highbit_pos(extensions[0]);
        return (a ? a + 64 : b) + 1;
    }
    uint32_t res = 0;
    for (int32_t i = num_extension_words_ - 1; i >= 0; i--)
        res = std::max(res, (extensions[i] ? highbit_pos(extensions[i]) + 64 * i + 1 : 0));
    return res;
}


inline void VALECounters::shift_extensions_left_from_pos(uint64_t extensions[], uint32_t pos,
                                                         uint32_t shamt) const {
    assert(shamt < 64);   // Shifting more than a word is not implemented.
    if (num_extension_words_ == 1) {
        const uint64_t a = extensions[0] & BITMASK(pos);
        const uint64_t b = extensions[0] & (BITMASK(64) << pos);
        extensions[0] = (b << shamt) | a;
    }
    else if (num_extension_words_ == 2) {
        if (pos < 64) {
            const uint64_t b = extensions[0] >> pos;
            extensions[1] <<= shamt;
            extensions[1] |= (64 < pos + shamt ? b << (pos + shamt - 64) : b >> (64 - pos - shamt));
            extensions[0] &= BITMASK(pos);
            extensions[0] |= (pos + shamt >= 64 ? 0ULL : (b << (pos + shamt)));
        }
        else {
            const uint32_t new_pos = pos - 64;
            const uint64_t b = extensions[1] & (~BITMASK(new_pos));
            extensions[1] &= BITMASK(new_pos);
            extensions[1] |= b << shamt;
        }
    }
    else {
        const int32_t pos_word = pos / 64;
        for (int32_t i = num_extension_words_ - 1; i > pos_word; i--) {
            const uint64_t prev_extension = extensions[i - 1]
                    & (~BITMASK(std::max(0, static_cast<int32_t>(pos) - (i - 1) * 64)));
            extensions[i] = (extensions[i] << shamt) | (prev_extension >> (64 - shamt));
        }
        const uint64_t a = extensions[pos_word] & BITMASK(pos % 64);
        const uint64_t b = extensions[pos_word] & (BITMASK(64) << (pos % 64));
        extensions[pos_word] = (b << shamt) | a;
    }
}


inline void VALECounters::shift_extensions_right_from_pos(uint64_t extensions[], uint32_t pos,
                                                          uint32_t shamt) const {
    if (num_extension_words_ == 1) {
        const uint64_t a = extensions[0] & BITMASK(pos);
        const uint64_t b = extensions[0] & (~BITMASK(pos));
        extensions[0] = ((b >> shamt) & (~BITMASK(pos))) | a;
    }
    else if (num_extension_words_ == 2) {
        if (pos < 64) {
            const bool end_in_second_word = pos + shamt >= 64;
            const uint32_t dead_a = end_in_second_word ? pos + shamt - 64 : 0U;
            const uint32_t shift_a = 64 - shamt + dead_a;
            const uint64_t a = (extensions[1] >> dead_a) & BITMASK(shamt);
            const uint64_t b = (end_in_second_word ? 0ULL : extensions[0] >> (pos + shamt));
            extensions[1] >>= shamt;
            extensions[0] &= BITMASK(pos);
            extensions[0] |= ((a << shift_a) | (b << pos));
        }
        else {
            const uint32_t new_pos = pos - 64;
            const uint64_t a = extensions[1] & (~BITMASK(new_pos + shamt));
            extensions[1] &= BITMASK(new_pos);
            extensions[1] |= a >> shamt;
        }
    }
    else {
        uint32_t running_prefix = pos % 64;
        for (uint32_t i = pos / 64; i < num_extension_words_; i++) {
            const uint64_t a = extensions[i] & BITMASK(running_prefix);
            const uint64_t b = extensions[i] & (~BITMASK(running_prefix));
            const uint64_t c = (i + 1 < num_extension_words_ ? extensions[i + 1] & BITMASK(shamt) : 0ULL);
            // Both of the shifted terms are masked to the bits at or above
            // `pos`: the bits carried in from the next word land at
            // `64 - shamt` upwards, which reaches *below* `pos` as soon as
            // `pos % 64 + shamt` passes 64 -- and everything below `pos` has
            // to survive this shift untouched.
            extensions[i] = (((c << (64 - shamt)) | (b >> shamt)) & ~BITMASK(running_prefix)) | a;
            running_prefix = 0;
        }
    }
}


inline uint32_t *VALECounters::setup_tails_array(const uint64_t *overflows_bitmap,
                                                 uint64_t *extensions,
                                                 uint32_t total_extension_len) {
    uint32_t *ptr = new uint32_t[counters_per_chunk_]{};
    uint32_t running_val = 0;   // The value of the extension being decoded.
    uint32_t running_pw = 1;    // Used to decode that value, which is in base 3.
    uint32_t running_rank = 0;  // Its rank among the pool's extensions.
    uint32_t overflows_pos = 0, running_overflows_count = 0;
    for (uint32_t i = 0; i < total_extension_len; i += extension_size) {
        const uint32_t fragment = (extensions[i / 64] >> (i % 64)) & BITMASK(extension_size);
        if (fragment == MAX_VALUE(extension_size)) {    // A terminator: store the extension in its tail.
            uint32_t ptr_pos = bit_select(overflows_bitmap[overflows_pos], running_rank - running_overflows_count);
            while (ptr_pos == 64) {
                running_overflows_count += __builtin_popcountll(overflows_bitmap[overflows_pos]);
                overflows_pos++;
                ptr_pos = bit_select(overflows_bitmap[overflows_pos], running_rank - running_overflows_count);
            }
            ptr[ptr_pos + 64 * overflows_pos] = running_val;
            running_val = 0;
            running_pw = 1;
            running_rank++;
            continue;
        }
        running_val = running_val + running_pw * fragment;
        running_pw *= BITMASK(extension_size);
    }

    // Store the low 48 bits of the pointer in the pool, and set the mode bit
    // at its very top to say the chunk has a tails array now.
    memset(extensions, 0, sizeof(extensions[0]) * num_extension_words_);
    extensions[0] = reinterpret_cast<uint64_t>(ptr) & BITMASK(extension_size * min_extension_count);
    extensions[num_extension_words_ - 1] |= 1ULL << (last_extension_word_bit_count_ - 1);
    chunks_with_tails_++;
    return ptr;
}


/******************************************************************
 * Reading and writing a counter.                                 *
 ******************************************************************/

inline uint64_t VALECounters::get_raw(uint64_t pos) const {
    const uint64_t chunk = pos / counters_per_chunk_;
    const uint32_t inter_chunk = pos - chunk * counters_per_chunk_;
    const uint8_t *chunk_ptr_ = chunk_ptr(chunk);

    // The stub.
    const uint64_t *read_word = reinterpret_cast<const uint64_t *>(chunk_ptr_
                                                    + word_update_byte_offset_[inter_chunk]);
    uint64_t res = (read_word[0] >> word_update_shamt_[inter_chunk]) & stub_mask_;

    // The extension, if the counter has one.
    const uint64_t *words = reinterpret_cast<const uint64_t *>(chunk_ptr_);
    if (!is_overflowing(words, inter_chunk))
        return res;

    const uint32_t extension_rank = get_extension_rank(words, inter_chunk);
    uint64_t extensions[max_extension_words];
    read_extensions(words, extensions);
    if ((extensions[num_extension_words_ - 1] >> (last_extension_word_bit_count_ - 1)) & 1) {
        const uint32_t *ptr = get_tails_ptr(extensions);
        res |= static_cast<uint64_t>(ptr[inter_chunk]) << stub_size_;
        return res;
    }

    uint64_t add_res = 0, add_pw = 1;
    for (uint32_t i = get_extension_pos(extensions, extension_rank); true; i += extension_size) {
        const uint32_t new_bits = (extensions[i / 64] >> (i % 64)) & BITMASK(extension_size);
        if (new_bits == MAX_VALUE(extension_size))
            break;
        add_res += new_bits * add_pw;
        add_pw *= MAX_VALUE(extension_size);
    }
    return res + (add_res << stub_size_);
}


inline void VALECounters::set_raw(uint64_t pos, uint64_t value) {
    assert(pos < counter_count_ && value <= MaxValue());
    const uint64_t chunk = pos / counters_per_chunk_;
    const uint32_t inter_chunk = pos - chunk * counters_per_chunk_;
    uint8_t *chunk_ptr_ = chunk_ptr(chunk);

    // The stub.
    uint64_t *write_word = reinterpret_cast<uint64_t *>(chunk_ptr_ + word_update_byte_offset_[inter_chunk]);
    write_word[0] &= ~(stub_mask_ << word_update_shamt_[inter_chunk]);
    write_word[0] |= (value & stub_mask_) << word_update_shamt_[inter_chunk];

    // The extension.
    uint64_t *words = reinterpret_cast<uint64_t *>(chunk_ptr_);
    const bool new_has_extension = value > MAX_VALUE(stub_size_);
    const bool old_has_extension = is_overflowing(words, inter_chunk);
    const int update_state = static_cast<int>(old_has_extension) * 2 + static_cast<int>(new_has_extension);
    if (update_state) {
        const uint32_t extension_rank = get_extension_rank(words, inter_chunk);
        uint64_t extensions[max_extension_words];
        read_extensions(words, extensions);

        const bool already_has_tails_ptr = has_tails_array(words);
        const uint32_t total_extension_len = (already_has_tails_ptr
                                                ? extension_size * num_extension_ + 1
                                                : get_extension_length(extensions));
        uint32_t new_extension_len = extension_size;
        const uint64_t extension_value = value >> stub_size_;
        for (uint64_t val = extension_value; val; val /= MAX_VALUE(extension_size))
            new_extension_len += extension_size;

        switch (update_state) {
            case 1: {   // Gained an extension.
                if (already_has_tails_ptr) {
                    get_tails_ptr(extensions)[inter_chunk] = extension_value;
                }
                else if (new_extension_len + total_extension_len > extension_size * num_extension_) {
                    setup_tails_array(words, extensions, total_extension_len)[inter_chunk] = extension_value;
                }
                else {
                    const uint32_t at = get_extension_pos(extensions, extension_rank);
                    shift_extensions_left_from_pos(extensions, at, new_extension_len);
                    uint64_t tmp_val = extension_value, i;
                    for (i = at; tmp_val; tmp_val /= MAX_VALUE(extension_size), i += extension_size)
                        extensions[i / 64] |= ((tmp_val % MAX_VALUE(extension_size)) << (i % 64));
                    extensions[i / 64] |= (MAX_VALUE(extension_size) << (i % 64));
                }
                break;
            }
            case 2: {   // Lost its extension.
                if (already_has_tails_ptr) {
                    get_tails_ptr(extensions)[inter_chunk] = 0;
                }
                else {
                    const uint32_t at = get_extension_pos(extensions, extension_rank);
                    const uint32_t old_extension_len =
                            std::min(get_extension_pos(extensions, extension_rank + 1), total_extension_len) - at;
                    shift_extensions_right_from_pos(extensions, at, old_extension_len);
                }
                break;
            }
            case 3: {   // Kept its extension, with a new value.
                const uint32_t at = get_extension_pos(extensions, extension_rank);
                const uint32_t old_extension_len =
                        std::min(get_extension_pos(extensions, extension_rank + 1), total_extension_len) - at;
                if (already_has_tails_ptr) {
                    get_tails_ptr(extensions)[inter_chunk] = extension_value;
                }
                else if (new_extension_len + total_extension_len - old_extension_len
                            > extension_size * num_extension_) {
                    setup_tails_array(words, extensions, total_extension_len)[inter_chunk] = extension_value;
                }
                else {
                    shift_extensions_right_from_pos(extensions, at, old_extension_len);
                    shift_extensions_left_from_pos(extensions, at, new_extension_len);
                    uint64_t tmp_val = extension_value, i;
                    for (i = at; tmp_val; tmp_val /= MAX_VALUE(extension_size), i += extension_size)
                        extensions[i / 64] |= ((tmp_val % MAX_VALUE(extension_size)) << (i % 64));
                    extensions[i / 64] |= (MAX_VALUE(extension_size) << (i % 64));
                }
                break;
            }
        }
        write_extensions(words, extensions);
    }
    set_overflowing(words, inter_chunk, new_has_extension);
}


inline void VALECounters::increment_counter(uint64_t chunk, uint32_t inter_chunk) {
    uint8_t *chunk_ptr_ = chunk_ptr(chunk);

    // The common case: the stub absorbs the increment on its own.
    uint64_t *update_word = reinterpret_cast<uint64_t *>(chunk_ptr_ + word_update_byte_offset_[inter_chunk]);
    const uint64_t stub_mask = stub_mask_ << word_update_shamt_[inter_chunk];
    if ((update_word[0] & stub_mask) != stub_mask) {
        update_word[0] += 1ULL << word_update_shamt_[inter_chunk];
        return;
    }
    update_word[0] &= ~stub_mask;

    // The stub wrapped, so the carry goes into the extension.
    uint64_t *words = reinterpret_cast<uint64_t *>(chunk_ptr_);
    const bool has_extension = is_overflowing(words, inter_chunk);
    const uint32_t extension_rank = get_extension_rank(words, inter_chunk);
    uint64_t extensions[max_extension_words];
    read_extensions(words, extensions);
    const bool already_has_tails_ptr = has_tails_array(words);
    const uint32_t total_extension_len = (already_has_tails_ptr ? extension_size * num_extension_ + 1
                                                                : get_extension_length(extensions));
    if (already_has_tails_ptr) {
        get_tails_ptr(extensions)[inter_chunk]++;
    }
    else if (has_extension) {
        const uint32_t at = get_extension_pos(extensions, extension_rank);
        uint32_t i = at;
        uint64_t fragment = (extensions[i / 64] >> (i % 64)) & BITMASK(extension_size);
        bool absorbed = false, should_add_extension = true;
        do {
            absorbed = (fragment != MAX_VALUE(extension_size) - 1);
            should_add_extension &= !absorbed;
            extensions[i / 64] += (1ULL + (absorbed ? 0 : -MAX_VALUE(extension_size))) << (i % 64);
            i += extension_size;
            fragment = (extensions[i / 64] >> (i % 64)) & BITMASK(extension_size);
        } while ((!absorbed) & (fragment != MAX_VALUE(extension_size)));

        if (should_add_extension) {
            if (total_extension_len + extension_size > extension_size * num_extension_) {
                uint32_t *ptr = setup_tails_array(words, extensions, total_extension_len);
                ptr[inter_chunk] = 1;
                for (uint32_t j = at; j < i; j += extension_size)
                    ptr[inter_chunk] *= MAX_VALUE(extension_size);
            }
            else {
                shift_extensions_left_from_pos(extensions, i, extension_size);
                extensions[i / 64] += 1ULL << (i % 64);
            }
        }
    }
    else if (total_extension_len + 2 * extension_size > extension_size * num_extension_) {
        setup_tails_array(words, extensions, total_extension_len)[inter_chunk] = 1;
    }
    else {
        const uint32_t at = get_extension_pos(extensions, extension_rank);
        shift_extensions_left_from_pos(extensions, at, 2 * extension_size);
        extensions[at / 64] += 1ULL << (at % 64);
        extensions[(at + extension_size) / 64] |= MAX_VALUE(extension_size)
                                                    << ((at + extension_size) % 64);
    }

    set_overflowing_to_1(words, inter_chunk);
    write_extensions(words, extensions);
}


inline void VALECounters::decrement_counter(uint64_t chunk, uint32_t inter_chunk) {
    uint8_t *chunk_ptr_ = chunk_ptr(chunk);

    // The common case: the stub absorbs the decrement on its own.
    uint64_t *update_word = reinterpret_cast<uint64_t *>(chunk_ptr_ + word_update_byte_offset_[inter_chunk]);
    const uint64_t stub_mask = stub_mask_ << word_update_shamt_[inter_chunk];
    if ((update_word[0] & stub_mask) != 0) {
        update_word[0] -= 1ULL << word_update_shamt_[inter_chunk];
        return;
    }
    update_word[0] |= stub_mask;

    // The stub wrapped, so the borrow comes out of the extension.
    uint64_t *words = reinterpret_cast<uint64_t *>(chunk_ptr_);
    const uint32_t extension_rank = get_extension_rank(words, inter_chunk);
    uint64_t extensions[max_extension_words];
    read_extensions(words, extensions);

    if (has_tails_array(words)) {
        uint32_t *ptr = get_tails_ptr(extensions);
        ptr[inter_chunk]--;
        set_overflowing_to_0(words, inter_chunk, ptr[inter_chunk] == 0);
    }
    else {
        const uint32_t at = get_extension_pos(extensions, extension_rank);
        uint32_t i = at;
        uint64_t fragment = (extensions[i / 64] >> (i % 64)) & BITMASK(extension_size);
        bool absorbed = false, should_remove_extension = true;
        do {
            absorbed = (fragment != 0);
            should_remove_extension &= (!absorbed | (fragment == 1));
            extensions[i / 64] -= (1ULL + (absorbed ? 0 : -MAX_VALUE(extension_size))) << (i % 64);
            i += extension_size;
            fragment = (extensions[i / 64] >> (i % 64)) & BITMASK(extension_size);
        } while ((!absorbed) & (fragment != MAX_VALUE(extension_size)));

        should_remove_extension &= (fragment == MAX_VALUE(extension_size));
        if (should_remove_extension) {
            const uint64_t extensions_depleted = (i == at + extension_size);
            const uint32_t shamt = (extensions_depleted ? extension_size * 2 : extension_size);
            shift_extensions_right_from_pos(extensions, i - extension_size, shamt);
            set_overflowing_to_0(words, inter_chunk, extensions_depleted);
        }
        write_extensions(words, extensions);
    }
}


/******************************************************************
 * Shifting.                                                      *
 ******************************************************************/

inline void VALECounters::shift_chunk_bits_up(uint64_t *words, uint32_t start, uint32_t end,
                                              uint32_t shamt) {
    // Walk the range from the top down in word-sized bites, so that every
    // write lands above what the next read still needs.
    for (uint32_t done = 0, len = end - start; done < len; ) {
        const uint32_t take = std::min<uint32_t>(64, len - done);
        const uint32_t src = end - done - take;
        write_chunk_bits(words, src + shamt, take, read_chunk_bits(words, src, take));
        done += take;
    }
    write_chunk_bits(words, start, shamt, 0);
}


inline void VALECounters::shift_chunk_bits_down(uint64_t *words, uint32_t start, uint32_t end,
                                                uint32_t shamt) {
    for (uint32_t done = 0, len = end - start; done < len; ) {
        const uint32_t take = std::min<uint32_t>(64, len - done);
        const uint32_t src = start + done;
        write_chunk_bits(words, src - shamt, take, read_chunk_bits(words, src, take));
        done += take;
    }
    write_chunk_bits(words, end - shamt, shamt, 0);
}


inline void VALECounters::shift_within_chunk_up(uint64_t chunk, uint32_t lo, uint32_t hi) {
    assert(lo <= hi && hi + 1 < counters_per_chunk_);
    uint64_t *words = chunk_words(chunk);
    // The stubs, then the overflows bitmap. Both are contiguous bit ranges,
    // and both stay clear of the extension pool above them.
    shift_chunk_bits_up(words, counters_per_chunk_ + lo * stub_size_,
                        counters_per_chunk_ + (hi + 1) * stub_size_, stub_size_);
    shift_chunk_bits_up(words, lo, hi + 1, 1);
    // The extension pool needs no attention: it is ordered by the position of
    // the counters its entries belong to, and that order just survived. A
    // tails array is indexed by position, though, so it does have to move.
    if (has_tails_array(words)) {
        uint64_t extensions[max_extension_words];
        read_extensions(words, extensions);
        uint32_t *ptr = get_tails_ptr(extensions);
        memmove(ptr + lo + 1, ptr + lo, (hi - lo + 1) * sizeof(uint32_t));
        ptr[lo] = 0;
    }
}


inline void VALECounters::shift_within_chunk_down(uint64_t chunk, uint32_t lo, uint32_t hi) {
    assert(lo <= hi && lo > 0);
    uint64_t *words = chunk_words(chunk);
    shift_chunk_bits_down(words, counters_per_chunk_ + lo * stub_size_,
                          counters_per_chunk_ + (hi + 1) * stub_size_, stub_size_);
    shift_chunk_bits_down(words, lo, hi + 1, 1);
    if (has_tails_array(words)) {
        uint64_t extensions[max_extension_words];
        read_extensions(words, extensions);
        uint32_t *ptr = get_tails_ptr(extensions);
        memmove(ptr + lo - 1, ptr + lo, (hi - lo + 1) * sizeof(uint32_t));
        ptr[hi] = 0;
    }
}


inline void VALECounters::move_range_up(uint64_t first, uint64_t last) {
    const uint32_t cpc = counters_per_chunk_;
    const uint64_t first_chunk = first / cpc, last_chunk = last / cpc;

    // From the top down, so that a counter crossing out of a chunk finds the
    // slot it is headed for already vacated.
    for (uint64_t c = last_chunk + 1; c-- > first_chunk; ) {
        const uint32_t lo = (c == first_chunk ? first - c * cpc : 0);
        const uint32_t hi = (c == last_chunk ? last - c * cpc : cpc - 1);

        uint32_t hi_shift = hi;
        if (hi == cpc - 1) {
            // The chunk's last counter is headed for the next chunk. Taking it
            // out of this pool and putting it into that one is the only
            // extension work the whole shift does.
            const uint64_t crossing = get_raw(c * cpc + cpc - 1);
            set_raw(c * cpc + cpc - 1, 0);
            set_raw((c + 1) * cpc, crossing);
            if (lo == cpc - 1)
                continue;           // That counter was the only one to move.
            hi_shift = cpc - 2;
        }
        shift_within_chunk_up(c, lo, hi_shift);
    }
}


inline void VALECounters::move_range_down(uint64_t first, uint64_t last) {
    const uint32_t cpc = counters_per_chunk_;
    const uint64_t first_chunk = first / cpc, last_chunk = last / cpc;

    for (uint64_t c = first_chunk; c <= last_chunk; c++) {
        const uint32_t lo = (c == first_chunk ? first - c * cpc : 0);
        const uint32_t hi = (c == last_chunk ? last - c * cpc : cpc - 1);

        uint32_t lo_shift = lo;
        if (lo == 0) {
            // Symmetrically, the chunk's first counter is headed for the
            // previous chunk, which has already been shifted.
            const uint64_t crossing = get_raw(c * cpc);
            set_raw(c * cpc, 0);
            set_raw(c * cpc - 1, crossing);
            if (hi == 0)
                continue;
            lo_shift = 1;
        }
        shift_within_chunk_down(c, lo_shift, hi);
    }
}

}   // namespace sublime
