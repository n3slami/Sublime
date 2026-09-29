#pragma once

/** Set as soon as this header is seen; `SublimeMGNoTuning.hpp` checks for it. */
#define SUBLIME_VALECOUNTERS_INCLUDED 1

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
 * Three scales, and which one a caller sees
 * ---------------------------------------------------------------------------
 * With a min tree over them (`VALECounters(n, true)`, which is Sublime_MG's
 * tree configuration) the counters carry Misra-Gries' **lazy decrement** as
 * well as their counts, and they carry it in two pieces. There are three
 * numbers for one counter, and only the middle one is ever public:
 *
 *   - its **count**, what the caller's key has actually been seen;
 *   - its **value**, the count plus `L`, the lazy decrement. This is the scale
 *     `Get`, `Set`, `MinValue` and everything else here speaks. It does not
 *     move when a decrement is taken, which is the point: one `IncrementLazy`
 *     lowers every count by one and touches no memory at all;
 *   - its **stored form**, the value less however much of `L` has already been
 *     taken off the *group* it sits in. This never leaves the class.
 *
 * A group is an aligned block of `counters_per_group` counters -- 32, which is
 * exactly one level-5 subtree of the min tree, and that is not a coincidence
 * (see below). `group_applied_[g]` is how much of `L` group `g` has had
 * subtracted, and what it still owes is `L - group_applied_[g]`. A counter that
 * owes a lot is stored larger than it needs to be, so the owing is brought back
 * down by a **flush**: `MaybeFlushGroup` subtracts the difference from the 32
 * counters of one group, on demand, when a caller touches it and it has fallen
 * more than `1 << (stub_size - 2)` behind.
 *
 * Two things make that cheap, and both come from the group being a subtree:
 *
 *  - **A flush changes no count, so it changes nothing in the tree.** Not "only
 *    the nodes above the group": nothing. It subtracts a uniform amount from
 *    every stored counter of the group and adds it to `group_applied_[g]`, and
 *    the *value* of every one of them -- the scale the tree compares on -- comes
 *    out exactly as it went in. So a flush is a re-encoding of one or two cache
 *    lines and no more.
 *  - **Comparisons inside a group need no rebasing.** Two counters of one group
 *    owe the same amount, so it cancels: the bottom five levels of the climb
 *    compare stored forms directly. Only levels 6 and up read
 *    `group_applied_`, out of a table of `n / 8` bytes.
 *
 * A flush always holds **one** unit back, leaving the group owing at least one.
 * A stored zero means *this slot is empty*, and a count that has reached zero
 * -- an entry waiting to be evicted -- would otherwise store exactly that.
 *
 * ---------------------------------------------------------------------------
 * The min segment tree
 * ---------------------------------------------------------------------------
 * Misra-Gries needs the smallest count and a slot holding it, and it needs them
 * without a pass over the summary. The tree is a separate, packed bit array;
 * the counters stay the classic `n`.
 *
 * **An internal node stores the position of its subtree's minimum, as an offset
 * within that subtree** -- one bit at the bottom level, two the level above,
 * four above that. Rounded up to a power of two so that no field straddles a
 * 64-bit word, that is about 2.3 bits per counter, against the whole extra
 * counter (12-14 bits) that holding minima outright in a doubled array cost.
 * Level `l` holds `ceil(n / 2^l)` nodes, node `j` covering leaves
 * `[j * 2^l, (j+1) * 2^l)`; the last node of a level may cover a short range,
 * which costs one bounds check per level on the climb and no padding anywhere.
 * The root is not in the array at all: it is `min_pos_` and `min_value_`, two
 * members, so `MinSlot` and `MinValue` are a read each and nothing ever
 * descends from the root.
 *
 * What it costs to maintain is **one counter decode per level**, and the climbs
 * are split three ways to keep it there:
 *
 *  - **An increment** can only push its leaf *up*, so a node that does not name
 *    that leaf has a minimum strictly below what the leaf held and cannot move.
 *    The first level answers that with a one-bit field read and no decode at
 *    all, which is where three increments in four stop. Above it, a node that
 *    *does* name the leaf compares it against the other child's minimum -- and
 *    only for *inequality*, because the node naming this leaf already says it
 *    was the smaller of the two: a differing stub proves they differ, which
 *    proves the leaf was strictly below, which means one more still fits. See
 *    `provably_differs`.
 *  - **A value that fell**, which is every write below what the counter held
 *    and every admission into an empty slot, pulls minima down and cannot push
 *    any up, so no sibling is ever read: each level compares the new value
 *    against what the node names and stops at the first one that does not move.
 *  - **Anything else** -- a clear, a write above what was there -- reads the
 *    other child's minimum per level, since only it knows what replaces a
 *    minimum that rose.
 *
 * Either of the last two stops at the first node whose named leaf *and* value
 * are both unchanged; the value part matters, because a node can keep naming
 * the same leaf while that leaf's value moves under it.
 *
 * And one thing that is *not* here, because it is no longer needed: a range
 * minimum. The lazy decrement used to come off in one global pass, which left
 * the array on two scales at once and made "is any count zero" a pair of range
 * queries. Per-group application replaced it, and `MinValue` answers that on
 * its own again.
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

    /**
     * Counters per group, the unit the lazy decrement is applied in. A power of
     * two, so that a group is exactly one subtree of the min tree -- which is
     * what makes a flush invisible to the tree and comparisons within a group
     * free of rebasing. 32 of them carry one 32-bit `group_applied_` entry, so
     * the whole arrangement costs one bit per counter.
     */
    static constexpr uint32_t group_shift = 5;
    static constexpr uint32_t counters_per_group = 1u << group_shift;
    /**
     * How far behind a group is allowed to fall before a caller touching it
     * pays to bring it up to date, as a shift off the stub: the default is a
     * quarter of what a stub can hold.
     */
    static constexpr uint32_t default_flush_shift = 2;

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
    /** More levels than a tree over 2^47 counters could have. */
    static constexpr uint32_t max_levels = 48;

    /**
     * The widest the extension pool can get, in words. Sized off
     * `max_extension_count` so that the pool can be unpacked onto the stack
     * without a variable-length array. The one word of slack lets the
     * carry-propagation loops read one fragment past the pool's last one.
     */
    static constexpr uint32_t max_extension_words = extension_size * max_extension_count / 64 + 2;

    /**
     * The shape of the min tree: how many nodes each level holds, how wide its
     * offset fields are, and where in `tree_` they start.
     *
     * `size[0]` is the counter count. Levels `1 .. levels - 1` live in `tree_`;
     * level `levels` is the root, which holds one node and lives in `min_pos_`
     * and `min_value_` instead. An array of one counter has `levels == 1`, a
     * root covering the one leaf and nothing in `tree_` at all.
     */
    struct TreeShape {
        uint32_t levels = 0;
        uint64_t size[max_levels] = {};
        uint64_t start[max_levels] = {};
        uint8_t width[max_levels] = {};
    };

public:
    /**
     * Constructs an array of `counter_count` counters, all zero.
     *
     * @param with_min_tree Whether to maintain a min segment tree over them,
     * which adds about 2.3 bits a counter of tree and one more of lazy-decrement
     * bookkeeping: see the note at the top of this file. Without it the array is
     * exactly what it always was, and `Get`/`Set` are the stored values
     * themselves.
     */
    explicit VALECounters(uint64_t counter_count, bool with_min_tree = false) {
        tree_present_ = with_min_tree;
        allocate(counter_count, nullptr);
        setup_tree();
    }

    /**
     * Constructs an array of `counter_count` zero counters shaped the way
     * `like` is shaped. Used when a structure resizes: the counts about to be
     * copied over came out of an array with that tuning, so starting there
     * both skips an immediate retune and guarantees `MaxValue()` is wide
     * enough to hold every one of them.
     *
     * The lazy decrement comes across too, since the values about to be written
     * are on its scale. Every group starts owing exactly one -- the least that
     * keeps a count of zero from storing the zero that means *empty* -- so the
     * counters arrive as small as they can be whatever the source had applied.
     */
    VALECounters(uint64_t counter_count, const VALECounters& like) {
        tree_present_ = like.tree_present_;
        flush_shift_ = like.flush_shift_;
        lazy_ = like.lazy_;
        allocate_with(counter_count, like.counters_per_chunk_, like.stub_size_);
        setup_tree();
        const uint32_t applied = lazy_ > 0 ? static_cast<uint32_t>(lazy_ - 1) : 0;
        std::fill(group_applied_.begin(), group_applied_.end(), applied);
    }

    ~VALECounters() {
        free_tails();
        delete[] chunks_;
    }

    VALECounters(const VALECounters& other) {
        copy_tree_state_from(other);
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
        copy_tree_state_from(other);
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
        tree_present_ = other.tree_present_;
        tree_suspended_ = other.tree_suspended_;
        shape_ = other.shape_;
        tree_ = std::move(other.tree_);
        group_applied_ = std::move(other.group_applied_);
        lazy_ = other.lazy_;
        flush_shift_ = other.flush_shift_;
        group_flushes_ = other.group_flushes_;
        min_pos_ = other.min_pos_;
        min_value_ = other.min_value_;
        chunk_count_ = other.chunk_count_;
        chunks_with_tails_ = other.chunks_with_tails_;
        tail_retune_limit_ = other.tail_retune_limit_;
        adopt_tuning(other.counters_per_chunk_, other.stub_size_);
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

    /**
     * @returns The value of the counter at `pos` -- its count plus the lazy
     * decrement, which is the one scale this class is public about. Zero means
     * the position holds nothing.
     */
    uint64_t Get(uint64_t pos) const {
        const uint64_t stored = get_raw(pos);
        if (!tree_present_ || stored == 0)
            return stored;
        return stored + group_applied_[pos >> group_shift];
    }

    /** Sets the counter at `pos` to `value`, which must be at most `MaxValue()`. */
    void Set(uint64_t pos, uint64_t value) {
        if (!has_live_tree()) {
            set_raw(pos, to_stored(pos, value));
            return;
        }
        const uint64_t previous = Get(pos);
        set_raw(pos, to_stored(pos, value));
        after_write(pos, previous, value);
    }

    /**
     * The same, told what the counter holds now -- which saves a read, and
     * more than that tells the tree which way the minimum can have moved.
     *
     * Every caller with a tree over the counters knows: a kick has just read
     * the count it is carrying, an admission is writing `L + 1` into a slot it
     * knows to be empty, and an eviction is clearing the counter it asked the
     * tree for. `old_value` must be what `Get(pos)` would return.
     *
     * This is also the *only* way a counter that is changing hands should be
     * written. A cuckoo kick storing a new count in a slot and taking the old
     * one away is one call, not a clear and a store: two writes would cost two
     * climbs, and the first of them would put an empty leaf into the tree and
     * make the second pay for a full sibling-reading climb to get it out again.
     */
    void Set(uint64_t pos, uint64_t value, uint64_t old_value) {
        assert(!has_live_tree() || Get(pos) == old_value);
        set_raw(pos, to_stored(pos, value));
        if (has_live_tree())
            after_write(pos, old_value, value);
    }

    /**
     * @returns The largest value a counter can hold under the current tuning.
     * Everything above the stub goes into a 32-bit tail, so the ceiling moves
     * with the stub length.
     */
    uint64_t MaxValue() const {
        return (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) << stub_size_) | stub_mask_;
    }

    /**
     * Adds one to the counter at `pos`, carrying into its extension as needed.
     *
     * With a tree over the counters the position must not be empty: a count is
     * a value less `L`, so a counter that holds nothing has no count for this
     * to raise. `Set` is what puts something there.
     */
    void Increment(uint64_t pos) {
        increment_raw(pos);
        if (!has_live_tree())
            return;
        assert(!raw_is_zero(pos));
        // The bottom level, which is where most increments end: if the pair's
        // minimum is not this leaf then the minimum is strictly below what this
        // leaf held, and one more cannot have changed that. A one-bit field
        // read, and nothing decoded -- not this counter, not its sibling.
        if (shape_.levels > 0 && named_at(1, pos) != pos)
            return;
        const uint64_t value = Get(pos);
        climb<true>(pos, value, value - 1);
    }

    /** Takes one off the counter at `pos`, borrowing from its extension as needed. */
    void Decrement(uint64_t pos) {
        const uint64_t before = has_live_tree() ? Get(pos) : 0;
        decrement_raw(pos);
        if (!has_live_tree())
            return;
        const uint64_t value = Get(pos);
        if (value == 0)
            climb<false>(pos, 0, before);    // It emptied, so a minimum may have risen.
        else
            climb_fell(pos, value);
    }

    /**
     * Takes one off the counter at `pos` and says whether that emptied it.
     *
     * The test costs nothing beyond the decrement itself: a counter holds zero
     * exactly when it has no extension and its stub reads zero, so neither the
     * extension pool nor a tails array has to be touched to find out -- which
     * is what makes a Misra-Gries decrement pass, which asks this of every
     * counter it touches, worth doing in one sweep. That sweep is the
     * tree-less configuration, and this is its primitive.
     *
     * @returns True if the counter is now zero.
     */
    bool DecrementIsZero(uint64_t pos) {
        const uint64_t before = has_live_tree() ? Get(pos) : 0;
        const uint64_t chunk = pos / counters_per_chunk_;
        const uint32_t inter_chunk = pos - chunk * counters_per_chunk_;
        decrement_counter(chunk, inter_chunk);
        const bool empty = counter_is_zero(chunk, inter_chunk);
        assert(empty == (get_raw(pos) == 0));
        if (has_live_tree()) {
            if (empty)
                climb<false>(pos, 0, before);
            else
                climb_fell(pos, Get(pos));
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
        __builtin_prefetch(chunks_ + (pos / counters_per_chunk_) * cache_line_size_bytes);
    }

    /*
     * Prefetching a leaf's *ancestors* was tried under the previous tree, which
     * kept them in the counter array, and was not worth it: it cost between 0
     * and 6% at every depth from one ancestor to four. There is nothing to
     * hide -- the nodes are a few kilobytes and stay in L1, while the leaves
     * they start from span the whole array. The tree is now a packed bit array
     * an eighth of that size, so there is even less. The leaf prefetch above,
     * which `FindMatch` issues before it touches the table at all, is the one
     * that pays.
     */

    /** Zeroes every counter, keeping the current tuning. */
    void Reset() {
        free_tails();
        memset(chunks_, 0, static_cast<size_t>(chunk_count_) * cache_line_size_bytes);
        chunks_with_tails_ = 0;
        std::fill(tree_.begin(), tree_.end(), 0);
        std::fill(group_applied_.begin(), group_applied_.end(), 0);
        lazy_ = 0;
        group_flushes_ = 0;
        min_pos_ = 0;
        min_value_ = 0;
    }

    /**
     * @returns Whether VALE's tuning is derived at run time, or was fixed when
     * this was compiled. See `SublimeMGNoTuning.hpp.in`.
     */
    static constexpr bool TunesAtRuntime() {
        return tunes_at_runtime;
    }

    /* ------------------------------------------------------------------ */
    /* The lazy decrement.                                               */
    /* ------------------------------------------------------------------ */

    /**
     * @returns `L`: what every count is understated by in the values this class
     * reports, so that a count is `Get(pos) - L`.
     */
    uint64_t LazyDecrement() const {
        return lazy_;
    }

    /**
     * Takes one off every count at the cost of one increment.
     *
     * Nothing moves. A count is a value less `L`, so raising `L` lowers every
     * count at once -- and the values, which are the scale the tree compares
     * on and the scale a caller reads, are exactly where they were. The tree
     * and the minimum cache therefore need no attention either.
     */
    void IncrementLazy() {
        lazy_++;
    }

    /** @returns How much of `L` the group holding `pos` has yet to have applied. */
    uint64_t OwedAt(uint64_t pos) const {
        assert(tree_present_);
        return lazy_ - group_applied_[pos >> group_shift];
    }

    /** The largest amount any group owes. Instrumentation. */
    uint64_t MaxOwed() const {
        uint64_t worst = 0;
        for (uint32_t applied : group_applied_)
            worst = std::max(worst, lazy_ - applied);
        return worst;
    }

    uint64_t CountGroupFlushes() const {
        return group_flushes_;
    }

    /** How far behind a group may fall before a caller touching it flushes it. */
    uint64_t FlushThreshold() const {
        return uint64_t{1} << (stub_size_ > flush_shift_ ? stub_size_ - flush_shift_ : 1);
    }
    void SetFlushShift(uint32_t shift) {
        flush_shift_ = shift;
    }

    /**
     * Applies the outstanding lazy decrement to the group holding `pos`, if
     * that group has fallen more than `FlushThreshold()` behind.
     *
     * This is how `L` comes off the counters: not in one global pass, which was
     * the single most expensive operation the sketch made, but on demand, one
     * group at a time, paid for by a caller who is touching that group anyway.
     * The counters of a group live in one or two chunks, and the whole thing is
     * invisible to the tree -- see the note at the top of this file.
     *
     * @returns True if it flushed.
     */
    bool MaybeFlushGroup(uint64_t pos) {
        if (!tree_present_ || counter_count_ == 0)
            return false;
        const uint64_t group = std::min<uint64_t>(pos, counter_count_ - 1) >> group_shift;
        if (lazy_ - group_applied_[group] <= FlushThreshold())
            return false;
        flush_group(group);
        return true;
    }

    /** Brings every group up to date, for a caller about to reshape the array. */
    void FlushAllGroups() {
        for (uint64_t group = 0; group < group_applied_.size(); group++)
            flush_group(group);
    }

    /* ------------------------------------------------------------------ */
    /* The min segment tree.                                             */
    /* ------------------------------------------------------------------ */

    /** @returns Whether a min segment tree is maintained over the counters. */
    bool HasMinTree() const {
        return tree_present_;
    }

    /**
     * @returns The smallest non-empty counter's value, or 0 if every position
     * is empty. The root of the tree is a member, so this is one read.
     */
    uint64_t MinValue() const {
        assert(tree_present_);
        return min_value_;
    }

    /**
     * @returns A position whose counter holds `MinValue()`. Meaningless if that
     * is zero.
     *
     * Maintained by every climb rather than searched for. The tree stores
     * *positions*, so the climb that keeps the root right has the winner's
     * index in hand already and there is nothing left to descend for -- where
     * a tree of minima had to walk back down from the root to turn a value into
     * a slot.
     */
    uint64_t MinSlot() const {
        assert(tree_present_);
        return min_pos_;
    }

    /**
     * Stops maintaining the tree, for a caller about to write every counter
     * at once: one `RebuildTree` afterwards costs a single bottom-up pass,
     * where a climb per write would cost `n log n`.
     */
    void SuspendTree() {
        tree_suspended_ = true;
    }

    /**
     * Recomputes every node, and resumes maintenance.
     *
     * One decode per counter and no more: each level's winners are carried
     * forward in a scratch buffer, so a node compares two values it already has
     * rather than reading the two counters they came from.
     *
     * Note what does *not* need this. A retune re-encodes values it does not
     * change, and the tree is a function of the values alone -- it holds
     * positions, and positions do not move -- so retuning leaves it valid. Only
     * a resize, which renumbers every slot, invalidates it.
     */
    void RebuildTree() {
        tree_suspended_ = false;
        if (!tree_present_)
            return;
        if (counter_count_ == 0) {
            min_pos_ = 0;
            min_value_ = 0;
            return;
        }
        if (shape_.levels == 0)             // No counters at all.
            return;
        std::vector<uint64_t> best_idx(shape_.size[1]), best_val(shape_.size[1]);
        for (uint64_t j = 0; j < shape_.size[1]; j++) {
            uint64_t idx = 2 * j, val = Get(2 * j);
            if (2 * j + 1 < counter_count_) {
                const uint64_t other = Get(2 * j + 1);
                if (better(other, val)) {
                    idx = 2 * j + 1;
                    val = other;
                }
            }
            best_idx[j] = idx;
            best_val[j] = val;
            if (shape_.levels > 1)
                node_write(1, j, idx - (j << 1));
        }
        for (uint32_t l = 2; l <= shape_.levels; l++) {
            for (uint64_t j = 0; j < shape_.size[l]; j++) {
                uint64_t idx = best_idx[2 * j], val = best_val[2 * j];
                if (2 * j + 1 < shape_.size[l - 1] && better(best_val[2 * j + 1], val)) {
                    idx = best_idx[2 * j + 1];
                    val = best_val[2 * j + 1];
                }
                best_idx[j] = idx;
                best_val[j] = val;
                if (l < shape_.levels)
                    node_write(l, j, idx - (j << l));
            }
        }
        min_pos_ = best_idx[0];
        min_value_ = best_val[0];
    }

    /* ------------------------------------------------------------------ */
    /* Tuning.                                                           */
    /* ------------------------------------------------------------------ */

    /**
     * Re-derives VALE's parameters from the counters' current values and
     * rebuilds the array if too many chunks have spilled into tails arrays.
     *
     * @returns True if the array was rebuilt.
     */
    bool MaybeRetune() {
        if constexpr (!tunes_at_runtime)
            return false;               // The tuning is a compile-time constant.
        return ShouldRetune() && Retune();
    }

    /** @returns True if enough chunks hold tails arrays to warrant a retune. */
    bool ShouldRetune() const {
        return chunks_with_tails_ >= tail_retune_limit_;
    }

    /**
     * Re-derives VALE's parameters and rebuilds the array under them.
     *
     * A rebuild reads every counter out under one shape and writes it back
     * under another. The min tree rides across untouched: it names positions,
     * and a rebuild changes no position and no value.
     *
     * @param shrank Whether the counters have got smaller since the current
     * tuning was chosen. It tells the tuning it is free to follow them
     * wherever they went, rather than only in the direction that relieves an
     * overflow -- which is the thrash the default rule guards against.
     * @returns True if the array was rebuilt. Counters that have not shrunk,
     * with no better tuning to move to, rebuild nothing and switch further
     * retuning off rather than retrying it on every subsequent update.
     */
    bool Retune(bool shrank = false);

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

    /** @returns The number of counters. */
    uint64_t CountCounters() const {
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
             + static_cast<uint64_t>(chunks_with_tails_) * counters_per_chunk_ * sizeof(uint32_t)
             + tree_.size() * sizeof(uint64_t)
             + group_applied_.size() * sizeof(uint32_t);
    }

private:
    /** Rebuilds `other`'s counters under the given tuning. */
    VALECounters(const VALECounters& other, uint32_t counters_per_chunk, uint32_t stub_size) {
        copy_tree_state_from(other);
        allocate_with(other.counter_count_, counters_per_chunk, stub_size);
        copy_values_from(other);
    }

    /**
     * Takes on everything about `other` that is not the counters themselves:
     * the tree, its shape, the minimum cache, and the lazy decrement with how
     * much of it each group has had applied.
     *
     * All of it survives a rebuild verbatim. A rebuild re-encodes stored values
     * without changing them, and every one of these is a function of the values
     * and the positions, neither of which moves.
     */
    void copy_tree_state_from(const VALECounters& other) {
        tree_present_ = other.tree_present_;
        tree_suspended_ = other.tree_suspended_;
        shape_ = other.shape_;
        tree_ = other.tree_;
        group_applied_ = other.group_applied_;
        lazy_ = other.lazy_;
        flush_shift_ = other.flush_shift_;
        group_flushes_ = other.group_flushes_;
        min_pos_ = other.min_pos_;
        min_value_ = other.min_value_;
    }

    /**
     * Reads every counter of `other` out under its shape and writes it under
     * ours. Stored forms, not values: the groups and what they have had applied
     * come across unchanged, so there is nothing to rebase.
     */
    void copy_values_from(const VALECounters& other) {
        assert(counter_count_ == other.counter_count_);
        for (uint64_t i = 0; i < counter_count_; i++) {
            const uint64_t value = other.get_raw(i);
            if (value != 0)
                set_raw(i, value);
        }
    }

    uint64_t counter_count_ = 0;
    uint32_t chunk_count_ = 0;
    uint32_t chunks_with_tails_ = 0;
    uint32_t tail_retune_limit_ = 0;

    /* The min tree, and the lazy decrement it compares values on the scale of. */

    /** Whether a tree is maintained at all; see the constructor. */
    bool tree_present_ = false;
    /** Set while a caller is writing every counter; see `SuspendTree`. */
    bool tree_suspended_ = false;
    TreeShape shape_;
    /**
     * The tree: one packed field per internal node, holding the position of its
     * subtree's minimum as an offset within that subtree. Level `l`'s fields
     * are `shape_.width[l]` bits wide -- `l` rounded up to a power of two, so
     * that no field straddles a word -- and start at bit `shape_.start[l]`.
     */
    std::vector<uint64_t> tree_;
    /**
     * How much of `L` each group of `counters_per_group` counters has had
     * subtracted from its stored counters. One 32-bit entry per 32 counters.
     */
    std::vector<uint32_t> group_applied_;
    /** `L`, Misra-Gries' lazy decrement. Zero without a tree. */
    uint64_t lazy_ = 0;
    uint32_t flush_shift_ = default_flush_shift;
    uint64_t group_flushes_ = 0;
    /** The root: the smallest value and a position holding it. */
    uint64_t min_pos_ = 0;
    uint64_t min_value_ = 0;

    /* ------------------------------------------------------------------ */
    /* The scale conversions.                                            */
    /* ------------------------------------------------------------------ */

    /** @returns What a caller's `value` is stored as at `pos`. */
    uint64_t to_stored(uint64_t pos, uint64_t value) const {
        if (!tree_present_ || value == 0)
            return value;
        const uint64_t applied = group_applied_[pos >> group_shift];
        assert(value > applied);
        return value - applied;
    }

    /* ------------------------------------------------------------------ */
    /* Applying the lazy decrement, a group at a time.                    */
    /* ------------------------------------------------------------------ */

    /**
     * Subtracts everything group `group` owes -- bar one, so that a count of
     * zero does not store the zero that means *empty* -- from its counters, and
     * records that it has.
     *
     * The tree is not touched. Every counter of the group comes down by the same
     * amount and its `group_applied_` entry goes up by it, so every *value* in
     * the group is exactly what it was, and the tree compares values.
     */
    void flush_group(uint64_t group) {
        const uint64_t applied = group_applied_[group];
        if (lazy_ <= applied + 1)
            return;
        const uint64_t amount = lazy_ - applied - 1;
        const uint64_t lo = group << group_shift;
        const uint64_t hi = std::min<uint64_t>(lo + counters_per_group, counter_count_);
        for (uint64_t pos = lo; pos < hi; pos++)
            subtract_stored(pos, amount);
        group_applied_[group] = static_cast<uint32_t>(applied + amount);
        group_flushes_++;
    }

    /**
     * Takes `amount` off the stored form of the counter at `pos`, leaving an
     * empty position empty.
     *
     * A counter with no extension needs no borrow: it stores its count plus
     * what its group owes, which is strictly more than `amount`, so the whole
     * subtraction is one read-modify-write of the word its stub is in.
     */
    void subtract_stored(uint64_t pos, uint64_t amount) {
        const uint64_t chunk = pos / counters_per_chunk_;
        const uint32_t inter_chunk = pos - chunk * counters_per_chunk_;
        uint8_t *chunk_ptr_ = chunk_ptr(chunk);
        const uint64_t *words = reinterpret_cast<const uint64_t *>(chunk_ptr_);
        if (!is_overflowing(words, inter_chunk)) {
            uint64_t *word = reinterpret_cast<uint64_t *>(chunk_ptr_
                                                        + word_update_byte_offset_[inter_chunk]);
            const uint32_t shamt = word_update_shamt_[inter_chunk];
            const uint64_t stub = (word[0] >> shamt) & stub_mask_;
            if (stub == 0)
                return;                     // Empty, and owes nothing.
            assert(stub > amount);
            word[0] -= amount << shamt;
            return;
        }
        const uint64_t stored = get_raw(pos);
        assert(stored > amount);
        set_raw(pos, stored - amount);
    }

    /* ------------------------------------------------------------------ */
    /* The tree's shape and its fields.                                   */
    /* ------------------------------------------------------------------ */

    /** @returns `l` rounded up to a power of two: the field width of level `l`. */
    static uint32_t width_for_level(uint32_t l) {
        uint32_t width = 1;
        while (width < l)
            width <<= 1;
        return width;
    }

    /** Sizes the tree and the group table for `counter_count_` counters. */
    void setup_tree() {
        shape_ = TreeShape{};
        tree_.clear();
        group_applied_.clear();
        if (!tree_present_ || counter_count_ == 0)
            return;
        group_applied_.assign((counter_count_ + counters_per_group - 1) >> group_shift, 0);
        shape_.size[0] = counter_count_;
        uint64_t bits = 0;
        for (uint32_t l = 1; l < max_levels; l++) {
            shape_.size[l] = (shape_.size[l - 1] + 1) / 2;
            if (shape_.size[l] == 1) {
                // One node: this level is the root, and the root is a member.
                shape_.levels = l;
                break;
            }
            const uint32_t width = width_for_level(l);
            assert(width <= 32);
            shape_.width[l] = static_cast<uint8_t>(width);
            bits = (bits + width - 1) / width * width;      // So no field straddles a word.
            shape_.start[l] = bits;
            bits += shape_.size[l] * width;
        }
        tree_.assign((bits + 63) / 64, 0);
    }

    bool has_live_tree() const {
        return tree_present_ && !tree_suspended_;
    }

    /** @returns The field of node `j` at level `l`. */
    uint64_t node_get(uint32_t l, uint64_t j) const {
        const uint32_t width = shape_.width[l];
        const uint64_t at = shape_.start[l] + j * width;
        return (tree_[at >> 6] >> (at & 63)) & ((uint64_t{1} << width) - 1);
    }

    /**
     * Replaces that field, told what is in it. XOR-ing in the difference is one
     * read-modify-write of the word; clearing the field and then filling it is
     * two, the second waiting on the first.
     */
    void node_xor(uint32_t l, uint64_t j, uint64_t old_field, uint64_t field) {
        const uint32_t width = shape_.width[l];
        const uint64_t at = shape_.start[l] + j * width;
        tree_[at >> 6] ^= (old_field ^ field) << (at & 63);
    }

    /** The same, reading what is there itself. For the bulk rebuild. */
    void node_write(uint32_t l, uint64_t j, uint64_t field) {
        const uint32_t width = shape_.width[l];
        const uint64_t at = shape_.start[l] + j * width;
        const uint64_t mask = (uint64_t{1} << width) - 1;
        tree_[at >> 6] = (tree_[at >> 6] & ~(mask << (at & 63))) | (field << (at & 63));
    }

    /**
     * @returns The position of the minimum of the subtree rooted at node `j` of
     * level `l`. Level 0 is a leaf, which is its own minimum.
     */
    uint64_t subtree_min(uint32_t l, uint64_t j) const {
        return l == 0 ? j : (j << l) + node_get(l, j);
    }

    /**
     * @returns The position the node covering `pos` at level `l` names as its
     * minimum -- the root's cache once `l` reaches the top.
     */
    uint64_t named_at(uint32_t l, uint64_t pos) const {
        return l == shape_.levels ? min_pos_ : subtree_min(l, pos >> l);
    }

    /**
     * Which of two values is the smaller, where zero means *no counter here*
     * rather than a value of zero. A monitored key's counter is always at least
     * `L + 1`, so zero is unambiguous, and it leaves an empty position costing
     * a zero stub and nothing else -- where a `+inf` sentinel would give every
     * one of them an extension or a tails entry.
     */
    static bool better(uint64_t value, uint64_t than) {
        return value != 0 && (than == 0 || value < than);
    }

    /**
     * @returns Whether the counter at `pos` can be shown *not* to hold the
     * value `value`, from the word its stub is in and nothing else.
     *
     * The stub is the counter's low bits, so values that differ there differ
     * outright -- and both sides have to be brought onto the same scale first,
     * which for the low `stub_size` bits is one addition. Cheap where the
     * alternative is not: decoding an overflowing counter is a rank and a
     * select into its chunk's extension pool.
     */
    bool provably_differs(uint64_t pos, uint64_t value) const {
        const uint64_t chunk = pos / counters_per_chunk_;
        const uint32_t inter_chunk = pos - chunk * counters_per_chunk_;
        const uint64_t *word = reinterpret_cast<const uint64_t *>(chunk_ptr(chunk)
                                                        + word_update_byte_offset_[inter_chunk]);
        const uint64_t stub = (word[0] >> word_update_shamt_[inter_chunk]) & stub_mask_;
        return ((stub + group_applied_[pos >> group_shift]) & stub_mask_) != (value & stub_mask_);
    }

    /* ------------------------------------------------------------------ */
    /* The climbs.                                                        */
    /* ------------------------------------------------------------------ */

    /**
     * Maintains the tree after the counter at `pos` was written with `value`,
     * where it held `old_value`. Both are on the public scale, zero meaning
     * *nothing here*.
     */
    void after_write(uint64_t pos, uint64_t old_value, uint64_t value) {
        if (value == old_value)
            return;
        if (value != 0 && (old_value == 0 || value < old_value))
            climb_fell(pos, value);
        else
            climb<false>(pos, value, old_value);
    }

    /**
     * The climb for a counter whose value *fell* -- every write below what was
     * there, and every admission into an empty position, which writes `L + 1`
     * and `L + 1` is the least any live counter can be.
     *
     * A minimum that can only fall never needs the sibling: whatever a node
     * named before, it names this position now if the new value is below what
     * the node's minimum was and the same one otherwise. So each level is one
     * field read and at most one decode, and the first node that does not move
     * ends the climb -- nothing above a node that did not move can have moved.
     */
    void climb_fell(uint64_t pos, uint64_t value) {
        assert(value != 0);
        if (min_value_ == 0 || value <= min_value_) {
            // Below everything there is, which is the common case and the
            // cheapest: every node from here to the root names this position,
            // and not one of them has to be compared against anything. So the
            // climb decodes no counter at all, and its writes do not even
            // depend on each other -- where the loop below walks a chain of
            // dependent loads, a node read and then the counter it names.
            //
            // Misra-Gries admits at `L + 1`, which is the least a live counter
            // can be, so nearly every admission comes through here.
            for (uint32_t l = 1; l < shape_.levels; l++) {
                const uint64_t j = pos >> l;
                node_write(l, j, pos - (j << l));
            }
            min_pos_ = pos;
            min_value_ = value;
            return;
        }
        for (uint32_t l = 1; l <= shape_.levels; l++) {
            if (l == shape_.levels)
                break;
            const uint64_t j = pos >> l;
            const uint64_t field = node_get(l, j);
            const uint64_t named = (j << l) + field;
            if (named == pos)
                continue;           // Already the subtree's minimum, and it only fell.
            if (!better(value, Get(named)))
                return;             // The subtree's minimum is unchanged, and so is everything above.
            node_xor(l, j, field, pos - (j << l));
        }
        if (min_pos_ == pos || better(value, min_value_)) {
            min_pos_ = pos;
            min_value_ = value;
        }
    }

    /**
     * The climb for a counter whose value did not *fall*: one that rose, one
     * cleared, or one written with a value above what was there. `value` is
     * what the counter at `pos` now holds, zero if it is now empty, and
     * `old_value` is what it held before.
     *
     * Two facts do all the work here, and both follow from the value not having
     * fallen:
     *
     *  - **Only the nodes naming `pos` have anything to recompute.** A node
     *    whose minimum sits elsewhere has it at a position whose value has not
     *    moved, and that position was at or below what `pos` held, so it is
     *    still the smallest. The nodes naming `pos` are the ones from the bottom
     *    up to the first that does not, so the climb ends at that one -- reading
     *    one field to find out, and decoding nothing.
     *  - **Once the replacement is level with what `pos` held, no sibling above
     *    can beat it.** A node naming `pos` says every leaf beneath it is at or
     *    above what `pos` held, and every sibling subtree the climb has yet to
     *    fold is beneath one of those nodes. So the moment the running best
     *    equals `old_value` the folding stops, and the rest of the climb is a
     *    field read and a field write per level.
     *
     * The second is what makes an eviction affordable, and an eviction is what
     * this configuration does most. Misra-Gries evicts the smallest counter, so
     * every eviction clears a position that every one of its ancestors names --
     * a climb to the root, and without the shortcut a decoded counter at every
     * level of it. But a decrement empties *every* entry that was at one, which
     * on a skewed stream is hundreds of them at a time, so the replacement is
     * almost always level with what was cleared and almost always found in the
     * first level or two. Measured on kosarak at 16 KB: 10.7 decoded counters
     * per eviction before, 1.3 after.
     *
     * With `rose` the caller is promising the value went up by exactly one,
     * which buys one more thing: where the node still names `pos`, the two were
     * in that order, so proving the sibling's minimum merely *different* from
     * what `pos` held proves it was the larger and that one more still fits.
     * That is a stub read rather than a decode. See `provably_differs`.
     */
    template <bool rose>
    void climb(uint64_t pos, uint64_t value, uint64_t old_value) {
        if (shape_.levels == 0)             // No counters at all.
            return;
        // Splitting on whether this was the smallest counter in the array is
        // worth a function: if it was, every ancestor's minimum is the value it
        // held, so the climb reaches the root and not one field has to be read
        // to find that out. Misra-Gries evicts the smallest counter, so that is
        // the case nearly every eviction takes.
        if (pos == min_pos_)
            climb_from_minimum<rose>(pos, value, old_value);
        else
            climb_above_minimum<rose>(pos, value, old_value);
    }

    /** The climb above, for a position the root does not name. */
    template <bool rose>
    void climb_above_minimum(uint64_t pos, uint64_t value, uint64_t old_value) {
        uint64_t best_pos = pos, best_value = value;
        bool settled = false;
        for (uint32_t l = 1; l <= shape_.levels; l++) {
            const bool at_root = (l == shape_.levels);
            const uint64_t j = pos >> l;
            const uint64_t field = at_root ? 0 : node_get(l, j);
            if ((at_root ? min_pos_ : (j << l) + field) != pos)
                return;                     // This node's minimum was never `pos`.
            if (!settled) {
                fold_sibling<rose>(pos, l, old_value, best_pos, best_value);
                settled = (best_value == old_value);
            }
            if (at_root) {
                min_pos_ = best_pos;
                min_value_ = best_value;
                return;
            }
            if (best_pos != pos)
                node_xor(l, j, field, best_pos - (j << l));
        }
    }

    /**
     * The climb above, for the position the root names -- an eviction, almost
     * always.
     *
     * Nothing is read to decide where to stop, because there is nowhere to stop:
     * every ancestor of the smallest counter has that counter's value as its own
     * minimum, so every one of them has to be rewritten. Which also means the
     * old fields are of no interest, so the writes are plain rather than the
     * usual exchange, and none of them waits on a read.
     *
     * A node may have been naming some *other* position tied with this one, and
     * that is not a problem: what goes in is a position holding the subtree's
     * minimum, which is all a node ever claims.
     */
    template <bool rose>
    void climb_from_minimum(uint64_t pos, uint64_t value, uint64_t old_value) {
        uint64_t best_pos = pos, best_value = value;
        bool settled = false;
        for (uint32_t l = 1; l <= shape_.levels; l++) {
            if (!settled) {
                fold_sibling<rose>(pos, l, old_value, best_pos, best_value);
                settled = (best_value == old_value);
            }
            if (l == shape_.levels)
                break;
            const uint64_t j = pos >> l;
            node_write(l, j, best_pos - (j << l));
        }
        min_pos_ = best_pos;
        min_value_ = best_value;
    }

    /**
     * Folds the minimum of the other child at level `l` into the running best.
     *
     * Where the node still names `pos` and the caller has promised the value
     * only went up by one, the two were in that order, so proving the sibling's
     * minimum merely *different* from what `pos` held proves it was the larger
     * and that one more still fits -- a stub read rather than a decode. See
     * `provably_differs`.
     */
    template <bool rose>
    __attribute__((always_inline))
    void fold_sibling(uint64_t pos, uint32_t l, uint64_t old_value,
                      uint64_t& best_pos, uint64_t& best_value) const {
        const uint64_t sibling = (pos >> (l - 1)) ^ 1;
        if (sibling >= shape_.size[l - 1])
            return;
        const uint64_t other = subtree_min(l - 1, sibling);
        if (rose && best_pos == pos && provably_differs(other, old_value))
            return;
        const uint64_t other_value = Get(other);
        if (better(other_value, best_value)) {
            best_pos = other;
            best_value = other_value;
        }
    }

    /** Adds one to the counter at `pos`, leaving the tree alone. */
    void increment_raw(uint64_t pos) {
        const uint64_t chunk = pos / counters_per_chunk_;
        increment_counter(chunk, pos - chunk * counters_per_chunk_);
    }
    /** Takes one off the counter at `pos`, leaving the tree alone. */
    void decrement_raw(uint64_t pos) {
        const uint64_t chunk = pos / counters_per_chunk_;
        decrement_counter(chunk, pos - chunk * counters_per_chunk_);
    }

    /** Reads the stored form of the counter at `pos`. */
    uint64_t get_raw(uint64_t pos) const;
    /** Writes the stored form of the counter at `pos`. */
    void set_raw(uint64_t pos, uint64_t value);

    /** True if the counter at `pos` is empty. */
    bool raw_is_zero(uint64_t pos) const {
        const uint64_t chunk = pos / counters_per_chunk_;
        return counter_is_zero(chunk, pos - chunk * counters_per_chunk_);
    }

    /*
     * VALE's tuning, `(counters_per_chunk, stub_size)`. Normally the array
     * derives it from the counters it holds and re-derives it as they grow, and
     * these are members. Defining `VALE_FIXED_COUNTERS_PER_CHUNK` and
     * `VALE_FIXED_STUB_SIZE` *before* this header instead nails it down at
     * compile time, which folds every mask and shift in the bit-fiddling below
     * into a constant and compiles the retuning out: see
     * `SublimeMGNoTuning.hpp.in`, and `SublimeCMSNoTuning.hpp.in` for the same
     * idea done by duplicating a whole sketch.
     */
#ifdef VALE_FIXED_COUNTERS_PER_CHUNK
    static constexpr bool tunes_at_runtime = false;
    static constexpr uint32_t counters_per_chunk_ = VALE_FIXED_COUNTERS_PER_CHUNK;
    static constexpr uint32_t stub_size_ = VALE_FIXED_STUB_SIZE;
    static constexpr uint64_t stub_mask_ = (uint64_t{1} << stub_size_) - 1;
#else
    static constexpr bool tunes_at_runtime = true;
    uint32_t counters_per_chunk_ = 0;
    uint32_t stub_size_ = 0;
    uint64_t stub_mask_ = 0;
#endif
    uint32_t num_extension_ = 0;                /**< Fragments that fit in a chunk's extension pool. */
    uint32_t num_extension_words_ = 0;          /**< Words the pool is unpacked into. */
    uint32_t last_extension_word_bit_count_ = 0;/**< Bits of the pool held in its last word. */
    /* For each counter of a chunk, the word its stub starts in and its offset there. */
    uint8_t word_update_byte_offset_[max_counter_per_cache_line] = {};
    uint8_t word_update_shamt_[max_counter_per_cache_line] = {};
    uint8_t *chunks_ = nullptr;

    /*
     * Note: replacing `pos / counters_per_chunk_` with a precomputed reciprocal
     * -- the divisor is at most `max_counter_per_cache_line`, so one widening
     * multiply divides exactly -- was tried and is worth *nothing*. Not because
     * the division is cheap, but because in an optimized build there is no
     * division to remove: GCC already eliminates it, and the only `divq` left in
     * `bench_SublimeMG` are in the harness's hash maps. An isolated translation
     * unit does emit one for `Get`, which is what makes this worth writing down
     * -- the instruction is visible in the small, and gone in the large.
     * Measured on kosarak: inside the noise at both 16 KB and 256 KB, in both
     * configurations.
     */

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
#ifndef VALE_FIXED_COUNTERS_PER_CHUNK
        stub_mask_ = BITMASK(stub_size_);
#endif
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
    void compute_counter_len_cnt(uint32_t *counter_len_cnt) const {
        for (uint64_t i = 0; i < counter_count_; i++) {
            const uint64_t value = get_raw(i);
            counter_len_cnt[value ? highbit_pos(value) + 1 : 0]++;
        }
    }

    /**
     * Sizes and zeroes the array for `counter_count` counters under the tuning
     * `counter_len_cnt` calls for.
     *
     */
    void allocate(uint64_t counter_count, const uint32_t *counter_len_cnt) {
        if constexpr (!tunes_at_runtime) {
            allocate_with(counter_count, counters_per_chunk_, stub_size_);
            return;
        }
        auto [counters_per_chunk, stub_size] = tune_params(counter_len_cnt);
        allocate_with(counter_count, counters_per_chunk, stub_size);
    }

    /**
     * Takes on a tuning -- or, when the tuning is a compile-time constant,
     * checks that the caller is asking for the one this array was built with.
     * Everything that shapes an array goes through here.
     */
    void adopt_tuning(uint32_t counters_per_chunk, uint32_t stub_size) {
#ifdef VALE_FIXED_COUNTERS_PER_CHUNK
        // Nothing to take on, and nothing may differ: every shape this array is
        // ever given has to be the one it was compiled for. (`#ifdef` rather
        // than `if constexpr`, which outside a template still has to compile
        // the branch it discards -- and that branch assigns to a constant.)
        assert(counters_per_chunk == counters_per_chunk_ && stub_size == stub_size_);
        (void) counters_per_chunk;
        (void) stub_size;
#else
        counters_per_chunk_ = counters_per_chunk;
        stub_size_ = stub_size;
        stub_mask_ = BITMASK(stub_size);
#endif
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
    adopt_tuning(counters_per_chunk, stub_size);
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


inline bool VALECounters::Retune(bool shrank) {
    if constexpr (!tunes_at_runtime)
        return false;               // There is nothing to re-derive.
    uint32_t counter_len_cnt[length_histogram_size] = {};
    compute_counter_len_cnt(counter_len_cnt);
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
        // Counters the caller already shrank are rebuilt under the same shape
        // anyway, which re-derives the tail bookkeeping against the values they
        // now hold.
        counters_per_chunk = counters_per_chunk_;
        stub_size = stub_size_;
    }

    // The two shapes disagree about where everything lives, so the rebuild
    // reads each counter out under the old one and writes it back under the new.
    VALECounters rebuilt(*this, counters_per_chunk, stub_size);
    *this = std::move(rebuilt);
    return true;
}


inline bool VALECounters::RetuneIfNarrower() {
    if constexpr (!tunes_at_runtime)
        return false;                   // The tuning is a compile-time constant.
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

    // The stub, replaced with one read-modify-write rather than two. Clearing
    // the field and then writing it is `&=` followed by `|=`: two updates of the
    // same word, the second waiting on the first. XOR-ing in the *difference*
    // between what is there and what is wanted is one, and the word has to be
    // read either way.
    uint64_t *write_word = reinterpret_cast<uint64_t *>(chunk_ptr_ + word_update_byte_offset_[inter_chunk]);
    const uint32_t shamt = word_update_shamt_[inter_chunk];
    const uint64_t word = write_word[0];
    write_word[0] = word ^ ((((word >> shamt) ^ value) & stub_mask_) << shamt);

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

}   // namespace sublime
