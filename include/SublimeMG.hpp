#pragma once

/*
 * ============================================================================
 *
 *        SublimeMG
 *          The Sublime framework applied to the Misra-Gries summary.
 *
 * ============================================================================
 *
 * A Misra-Gries summary is a bounded set of monitored keys, each with a count.
 * Here the set of monitored keys is a `CuckooTable` -- a cuckoo filter
 * storing one fingerprint per monitored key -- and the counts are a
 * `VALECounters` array laid over it slot for slot, so that counter `i`
 * belongs to the fingerprint in slot `i`.
 *
 * Keeping the counts *beside* the fingerprints rather than inside the slots is
 * what lets each side be compact on its own terms: the fingerprints shrink as
 * the filter expands, and the counters, under VALE, are only as long as the
 * counts they actually hold. The table mirrors every slot it moves into the
 * counter array, so the two stay aligned without either knowing much about the
 * other.
 *
 * Every fingerprint in the table is of the same length, so a key corresponds
 * to exactly one entry and its count is that entry's counter. A fingerprint
 * stands for a whole family of keys, and gets shorter every time the table
 * doubles, so an entry may be holding a mix of several keys' occurrences --
 * which is what makes an estimate an over-estimate, never an under-estimate.
 *
 * ---------------------------------------------------------------------------
 * The insertion algorithm
 * ---------------------------------------------------------------------------
 * Misra-Gries has three cases, and an insertion falls into exactly one:
 *
 *   1. The key is monitored. Its count goes up by one.
 *   2. It is not, and there is room. It is admitted at a count of one.
 *   3. It is not, and there is no room. Every count comes down by one, any
 *      that reach zero are evicted, and the key takes one of the slots that
 *      frees up. If nothing reaches zero, the occurrence is dropped.
 *
 * There are two ways to apply case 3's decrement, and `use_min_tree` picks
 * between them.
 *
 * **The sweep** (the default) walks every entry, decrementing as it goes and
 * evicting whatever it empties on the spot. It is why the counters are asked
 * for `DecrementIsZero` rather than a decrement and a read: a VALE counter
 * holds zero exactly when it has no extension and its stub reads zero, so
 * finding the entries to evict costs nothing beyond the decrement that was
 * happening anyway -- no extension, and no tails array, is ever decoded for it.
 * It is `O(w)` in the monitored keys, and on a full summary it is the most
 * expensive thing the sketch does.
 *
 * **The min tree** makes it `O(log w)`. The decrement is not applied at all:
 * `VALECounters::IncrementLazy` raises `L`, a count is its counter less `L`, and
 * a count reaches zero exactly when the smallest counter catches up with `L`.
 * A min tree over the counters names the counter that has, so an eviction is a
 * read rather than a search, and `L` is taken off the counters themselves a
 * group of 32 at a time, on demand, by whichever insertion touches that group
 * next. Nothing here is `O(w)`; see `VALECounters`, which owns all of it.
 *
 * ---------------------------------------------------------------------------
 * The query algorithm
 * ---------------------------------------------------------------------------
 * A query is the counter of the one entry whose fingerprint matches the key,
 * or zero if neither of the key's two buckets holds a match. What it errs by is the occurrences
 * of the other keys sharing that fingerprint, which is why fingerprints are
 * kept as long as the space allows.
 *
 * ---------------------------------------------------------------------------
 * Keeping VALE tuned
 * ---------------------------------------------------------------------------
 * VALE's `(counters_per_chunk, stub_size)` pair is chosen from a histogram of
 * the counter values, exactly as `SublimeCMS` chooses it. Counts here move in
 * both directions, so the tuning is asked to follow them both ways:
 *
 *   - upwards, by `MaybeRetune`, when enough chunks have spilled into tails
 *     arrays -- the same trigger `SublimeCMS` uses, and the only one it needs;
 *   - downwards, by `RetuneIfNarrower`, when the decrement sweeps have left
 *     the counters small enough to want a narrower stub. That one costs a pass
 *     of its own, so it is asked only once every counter can have shrunk by a
 *     whole bit -- `VALECounters::ShrinkRetuneInterval` sweeps apart.
 *
 * ---------------------------------------------------------------------------
 * When the summary grows: the size function
 * ---------------------------------------------------------------------------
 * How many keys the summary should monitor is a function of how long the
 * stream has got. The paper writes that as the size function `W`, and Sublime
 * hands the sketch its *inverse*, `expansion_f`: given a size, the stream
 * length at which that size stops being enough. Storing the inverse is what
 * makes the test cheap -- `W` never has to be evaluated per insertion.
 *
 * So the rule "expand once `W(N)` exceeds what the table holds" is kept as a
 * precomputed threshold on `N`:
 *
 *     expansion_lim_ = expansion_f(Capacity())
 *
 * and an insertion that pushes `N` to it expands. `Capacity()` is the number
 * of keys the table can monitor -- its slot count discounted by the load
 * factor -- not the slots it has allocated, so the over-provisioning the hash
 * table needs to keep its runs short does not count as usable size.
 *
 * After expanding, the threshold is recomputed from the new `Capacity()`,
 * which is the size the expansion was expected to produce. The table decides
 * for itself whether that expansion is a deepening within the current period
 * or the doubling that ends one; the threshold only cares what it left
 * behind, which `CountSlotsAfterExpansion` predicts beforehand so that a table
 * that cannot grow any further can stop testing.
 *
 * What `N` counts is a choice, and it is the template parameter
 * `expand_on_error_inducing_insertions`. Not every insertion costs the summary
 * anything: one whose key is already monitored just increments a counter it
 * already had, exactly as an exact counter would, and leaves every estimate as
 * good as it was. It is the insertion that matches *nothing* that costs --
 * either it is admitted, adding an entry whose fingerprint the other keys now
 * have to share, or the summary is full and something has to be decremented
 * for it. So it is those, counted in `error_inducing_`, that the threshold is
 * tested against by default; passing `false` tests it against every insertion
 * instead.
 *
 * The measure only matters relative to `expansion_f`, which is written to suit
 * it. For a skewed stream the two are far apart -- a heavy hitter contributes
 * once and then never again -- so the same `W` reads very differently, and the
 * default grows the summary only as fast as the stream is actually giving it
 * trouble. It also settles a case the total would get wrong: a summary that
 * never fills answers exactly, and under the default it cannot expand at all,
 * since the cap in `grow_to_fit` sits at one slot per error-inducing
 * insertion and it already has one.
 *
 * Misra-Gries has no deletions, so `N` never falls and the summary never
 * contracts on its own. `Contract` is there for a caller that wants it, and
 * recomputes the threshold like an expansion does. Passing no `expansion_f`
 * at all leaves the summary at a fixed size, which is plain Misra-Gries.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "CuckooTable.hpp"
#include "VALECounters.hpp"

namespace sublime {

/**
 * @tparam expand_on_error_inducing_insertions Whether the size function is
 * tested against the error-inducing insertions alone (the default) or against
 * every insertion. See the note on the size function at the top of this file.
 * @tparam Table The monitored-key set, which is `CuckooTable`. It is a
 * parameter so that another table can be dropped in, not because there is a
 * second one: Sublime_MG only ever asks it for the ~20 methods below.
 * @tparam use_min_tree Whether to maintain a min segment tree over the counters
 * and decrement lazily, which trades about three bits a counter for an eviction
 * that costs `O(log w)` instead of a sweep. Off by default. See the note on the
 * decrement above.
 */
template <bool expand_on_error_inducing_insertions = true, bool use_min_tree = false,
          typename Table = CuckooTable>
class SublimeMG {
    friend class SublimeMGTest;

public:
    /** The monitored-key set this was instantiated over. */
    using table_type = Table;
    using hashmode = typename Table::hashmode;

    /** Signals that the key passed in has already been hashed. */
    static constexpr uint32_t flag_key_is_hash = Table::flag_key_is_hash;

    /* Status codes. */
    static constexpr int32_t err_no_space = Table::err_no_space;
    /** The key has no fingerprint in the table, so there is no count to touch. */
    static constexpr int32_t err_not_monitored = Table::err_doesnt_exist;

    /**
     * @param nslots The number of slots the fingerprint table holds. Need not
     * be a power of two. `Capacity()` is a shade under this.
     * @param key_bits The number of bits of the hash the table uses.
     * @param hash_mode The hashing mode, see `CuckooTable::hashmode`.
     * @param seed The seed of the hash function.
     * @param growth_coefficient `r`, the growth coefficient of Stretching.
     * @param expansion_f The inverse of the size function `W` of the paper:
     * given a number of monitored keys, the stream length at which that many
     * stops being enough. Leave it empty to keep the summary at a fixed size,
     * which is plain Misra-Gries.
     */
    SublimeMG(uint64_t nslots, uint64_t key_bits, hashmode hash_mode, uint32_t seed,
              uint32_t growth_coefficient = 1,
              std::function<uint64_t(double)> expansion_f = {}):
            table_{nslots, key_bits, hash_mode, seed, growth_coefficient},
            expansion_f_{std::move(expansion_f)} {
        table_.EnableCounters(use_min_tree);
        retarget();
    }

    /**
     * Counts one more occurrence of `key`, applying whichever of the three
     * Misra-Gries cases fits.
     *
     * @returns 0, or a negative status code.
     */
    int32_t Insert(uint64_t key, uint8_t flags = 0) {
        const int32_t res = insert_one(key, flags);
        if constexpr (use_min_tree)
            drain_emptied();
        return res;
    }

    /**
     * Estimates the frequency of `key`: the counter of the one stored
     * fingerprint matching it.
     *
     * That fingerprint stands for a whole family of keys -- a shorter one, in
     * a table that has expanded, for a larger family -- so the entry may be
     * holding another key's occurrences, or a mix of several keys'. But `key`'s
     * own occurrences are certainly among them, so the estimate never falls
     * short of the count Misra-Gries itself would hold.
     *
     * @returns The estimated count of `key`, or 0 if nothing matches it.
     */
    uint64_t Query(uint64_t key, uint8_t flags = 0) const {
        const int64_t pos = table_.FindMatch(key, flags);
        if (pos < 0)
            return 0;
        // With the tree the counters hold each count plus the lazy decrement,
        // which is the scale `VALECounters` is public about; without it they
        // hold the count outright and the subtraction below is of zero.
        const uint64_t value = table_.GetCounters()->Get(pos);
        const uint64_t owed = table_.GetCounters()->LazyDecrement();
        return value > owed ? value - owed : 0;
    }

    /** @returns True if some stored fingerprint matches `key`. */
    bool IsMonitored(uint64_t key, uint8_t flags = 0) const {
        return table_.FindMatch(key, flags) >= 0;
    }

    /** Empties the summary, keeping its shape. */
    void Reset() {
        table_.Reset();   // Resets the counters, and the lazy decrement, with it.
        total_decrements_ = 0;
        passes_since_shrink_check_ = 0;
        n_ = 0;
        error_inducing_ = 0;
        retarget();
    }

    /**
     * Grows the summary, keeping every monitored key and its count, and with
     * it the number of keys it can monitor.
     *
     * @returns The number of fingerprints afterwards, or a negative status
     * code.
     */
    int64_t Expand() {
        const int64_t res = table_.Expand();
        if (res >= 0)
            retarget();
        return res;
    }

    /**
     * Shrinks the summary. Note that this does *not* evict anything: it can
     * leave more keys monitored than `Capacity()` allows, and the next
     * insertions that find it full will bring it back down.
     *
     * @returns The number of fingerprints afterwards, or a negative status code.
     */
    int64_t Contract() {
        const int64_t res = table_.Contract();
        if (res >= 0)
            retarget();
        return res;
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
     * @returns How many times every count came down by one. This is the
     * classic Misra-Gries parameter: no key's count is understated by more
     * than this. With the min tree it is the lazy decrement; without it, the
     * number of sweeps.
     */
    uint64_t CountDecrements() const {
        return total_decrements_;
    }
    /**
     * @returns `L`, the decrement every counter carries with it. Always zero
     * without the min tree, where a decrement is applied on the spot instead.
     */
    uint64_t GetLazyDecrement() const {
        return table_.GetCounters()->LazyDecrement();
    }
    /** @returns `N`, the number of insertions the summary has seen. */
    uint64_t GetStreamLength() const {
        return n_;
    }
    /**
     * @returns How many insertions found no fingerprint matching their key,
     * and so could not simply be counted. These are the ones that cost
     * accuracy; the rest only ever incremented a counter the summary already
     * had.
     */
    uint64_t CountErrorInducingInsertions() const {
        return error_inducing_;
    }
    /**
     * @returns The measure the size function is actually tested against --
     * whichever of the two above `expand_on_error_inducing_insertions` selects.
     */
    uint64_t SizeMeasure() const {
        if constexpr (expand_on_error_inducing_insertions)
            return error_inducing_;
        else
            return n_;
    }
    /**
     * @returns The stream length at which the summary expands next, or
     * `UINT64_MAX` if it has no size function or has run out of room to grow.
     */
    uint64_t GetExpansionLimit() const {
        return expansion_lim_;
    }
    /**
     * Turns VALE's *re-tuning* on or off -- the passes that re-derive
     * `(counters_per_chunk, stub_size)` from the counts as they move, upwards
     * when chunks spill into tails arrays and downwards when the decrements
     * have left every counter narrower. With it off the counter array keeps
     * the tuning it was constructed with for the life of the summary, which is
     * what a measurement of what the tuning is worth wants.
     *
     * It does not touch the rebuild that merges the lazy decrement out: that
     * one borrows the same machinery but is the algorithm, not the tuning.
     */
    void SetVALERetuning(bool on) {
        vale_retuning_ = on;
    }
    bool GetVALERetuning() const {
        return vale_retuning_;
    }

    /** @returns How many times the size function has grown the summary. */
    uint64_t CountExpansions() const {
        return table_.GetExpansionCount();
    }
    /** @returns The bytes held by the fingerprints and the counters. */
    uint64_t SizeInBytes() const {
        return table_.SizeInBytes() + table_.GetCounters()->SizeInBytes();
    }
    const Table& GetTable() const {
        return table_;
    }
    const VALECounters& Counters() const {
        return *table_.GetCounters();
    }

private:
    /*
     * The bare Misra-Gries primitives. Not public: each one can leave the
     * summary in a state the algorithm itself would never produce -- over
     * capacity, or holding a count the table never saw -- and `Insert` and
     * `Query` are what a caller is meant to have.
     */

    /**
     * Starts monitoring `key`, whether or not there is room: its count goes up
     * by one, from zero if nothing matched it. The bare case-2 primitive.
     *
     * @returns 0, or `err_no_space` if the table is full.
     */
    int32_t StartMonitoring(uint64_t key, uint8_t flags = 0) {
        const int64_t pos = table_.FindMatch(key, flags);
        if (pos >= 0) {
            table_.GetCounters()->Increment(pos);
            return 0;
        }
        return admit(key, flags);
    }

    /**
     * Stops monitoring `key`: drops the stored fingerprint matching it along
     * with its count. The bare eviction primitive.
     *
     * @returns 0, or `err_not_monitored` if no fingerprint matches `key`.
     */
    int32_t StopMonitoring(uint64_t key, uint8_t flags = 0) {
        return table_.Delete(key, flags) < 0 ? err_not_monitored : 0;
    }

    Table table_;
    /** How many times every count has come down by one. */
    uint64_t total_decrements_ = 0;
    /** Decrement passes since VALE was last asked to follow the counters down. */
    uint64_t passes_since_shrink_check_ = 0;
    /** Whether VALE may re-derive its tuning; see `SetVALERetuning`. */
    bool vale_retuning_ = true;
    /**
     * How many emptied entries *any* insertion may evict on its way out, with
     * the min tree in use. See `drain_emptied`, which is where this
     * configuration's tail was hiding.
     */
    static constexpr uint64_t drain_batch = 2;
    /** Stands for "no count at all", above anything a counter can hold. */
    static constexpr uint64_t no_count = std::numeric_limits<uint64_t>::max();
    /** The threshold of a summary that will not expand again. */
    static constexpr uint64_t never_expands = std::numeric_limits<uint64_t>::max();

    /** `N`, the length of the stream so far. */
    uint64_t n_ = 0;
    /** How many of those insertions matched no stored fingerprint. */
    uint64_t error_inducing_ = 0;
    /**
     * The inverse of the paper's size function `W`, mapping a number of
     * monitored keys to the stream length at which that many stops being
     * enough. Empty if the summary is to stay the size it was built at.
     */
    std::function<uint64_t(double)> expansion_f_;
    /** The value `n_` has to reach for the summary to expand. */
    uint64_t expansion_lim_ = never_expands;

    /**
     * Recomputes the expansion threshold from the size the summary now has.
     * `expansion_f` is the inverse of `W`, so plugging the current capacity in
     * gives exactly the stream length at which `W` outgrows it; called after
     * every resize, the threshold that comes out is the one for the size that
     * resize was expected to produce.
     */
    void retarget() {
        if (!expansion_f_ || table_.CountSlotsAfterExpansion() <= table_.CountSlots()) {
            // No size function, or no room left to grow into: stop testing.
            expansion_lim_ = never_expands;
            return;
        }
        expansion_lim_ = expansion_f_(static_cast<double>(Capacity()));
    }

    /**
     * Expands until the summary is as large as the size function asks for.
     * Normally that is one expansion, or none -- `n_` climbs by one per
     * insertion -- but a size function steep enough to skip a size is handled
     * by going round again rather than by lagging behind it.
     *
     * The loop is capped at a summary large enough to hold a slot per element
     * of the stream so far, which is the point at which Misra-Gries is just
     * exact counting and no size function can want more. Without that cap the
     * loop's termination would rest on the caller's `expansion_f` rising above
     * `n_` eventually, and one that does not -- or a threshold left stale by a
     * bug -- would double the table until the machine ran out of memory. The
     * cap costs one comparison and bounds the whole loop at `log2(n_)` turns.
     */
    void grow_to_fit() {
        while (SizeMeasure() >= expansion_lim_ && Capacity() < SizeMeasure()) {
            const uint64_t slots_before = table_.CountSlots();
            if (Expand() < 0 || table_.CountSlots() <= slots_before) {
                // Nothing more to be had from expanding; stop testing for it.
                expansion_lim_ = never_expands;
                return;
            }
        }
    }

    /**
     * One Misra-Gries insertion, without the drain `Insert` runs afterwards.
     */
    int32_t insert_one(uint64_t key, uint8_t flags) {
        // Tested before this insertion is counted, so that the measure the
        // threshold reads is complete: whether *this* key is error-inducing is
        // not known until it has been looked up, and the lookup has to happen
        // after any expansion, which moves every entry.
        if (SizeMeasure() >= expansion_lim_)
            grow_to_fit();
        n_++;

        const int64_t pos = table_.FindMatch(key, flags);
        if (pos >= 0) {                             // Case 1: already monitored.
            table_.GetCounters()->Increment(pos);
            rebuild_if_vale_asks();
            return 0;
        }
        // Nothing in the table matched, so this occurrence is one the summary
        // could not simply count -- see the size function note up top.
        error_inducing_++;

        if (CountMonitored() < Capacity())          // Case 2: room to admit it.
            return admit(key, flags);

        // Case 3: every count comes down by one, and the occurrence that paid
        // for that takes one of the slots it emptied, if it emptied any.
        if constexpr (use_min_tree) {
            VALECounters *counters = table_.GetCounters();
            // Nothing is at zero yet, so this occurrence has to pay for the
            // decrement itself. When something *is* already at zero -- a key
            // tied with the one a previous arrival evicted -- the decrement
            // has been paid for and this occurrence simply takes the slot,
            // which is what the sweep did when one pass emptied several.
            if (smallest_count() > 0) {
                counters->IncrementLazy();
                total_decrements_++;
            }
            // The decrement is now owed by every counter in the summary. This
            // insertion pays part of that debt off: the group of counters the
            // arriving key hashes to, if it has fallen far enough behind to be
            // storing counts wider than it needs to. Every case-3 insertion
            // lands on a group the hash chose, so the debt comes off everywhere
            // without anything ever making a pass over the whole array.
            counters->MaybeFlushGroup(table_.PrimarySlotOf(key, flags));
            if (smallest_count() != 0)
                return 0;                           // Nothing has reached zero.
            // One slot, for this arrival, and no more: the rest of what the
            // decrement emptied is cleared out a couple at a time by
            // `drain_emptied`, on the way out of the insertions that follow.
            // The tree named the slot and said what its counter holds, so the
            // eviction costs the array no decode of its own.
            evict_at(counters->MinSlot(), counters->MinValue());
            return admit(key, flags);
        }
        else {
            return decrement_pass() > 0 ? admit(key, flags) : 0;
        }
    }

    /**
     * Case 3. Takes one off every count in one sweep of the entries, and
     * evicts the ones that reach zero.
     *
     * An entry is evicted the moment the sweep empties it: a deletion clears
     * its slot and moves nothing else, and the iterator has already passed
     * that slot. (A quotient filter would have slid the rest of the cluster
     * down over the hole, moving slots the sweep had yet to reach, which is
     * why this used to collect its victims and delete them afterwards.) The
     * sweep also leaves every counter one smaller, which is what lets VALE
     * follow them downwards -- see the note on tuning at the top of this file.
     *
     * @returns The number of entries evicted, i.e. the slots this freed.
     */
    uint64_t decrement_pass() {
        VALECounters *counters = table_.GetCounters();
        uint64_t evicted = 0;
        for (auto it = table_.begin(); it != table_.end(); ++it) {
            if (counters->DecrementIsZero(it.slot())) {
                table_.DeleteSlot(it.bucket(), it.slot());
                evicted++;
            }
        }
        total_decrements_++;

        // Every counter has just come down by one, so after enough passes they
        // may all fit in a narrower stub. Asking costs a pass of its own, so it
        // is asked only once they can have shrunk by a whole bit.
        if (vale_retuning_
                && ++passes_since_shrink_check_ >= counters->ShrinkRetuneInterval()) {
            passes_since_shrink_check_ = 0;
            counters->RetuneIfNarrower();
        }
        return evicted;
    }

    /**
     * Drops the entry in `slot`, which with the min tree in use is the one the
     * tree named as the smallest -- the one whose count has reached zero.
     *
     * The tree hands back a slot; the table wants the bucket it belongs to as
     * well, which `BucketOfSlot` gives for nothing (it is `slot / depth`). The
     * removal clears that one counter and moves no other, so the tree has one
     * leaf to repair and nothing else.
     */
    void evict_at(uint64_t slot) {
        table_.DeleteSlot(table_.BucketOfSlot(slot), slot);
    }

    /** The same, told what the counter holds, which saves the array a decode. */
    void evict_at(uint64_t slot, uint64_t value) {
        table_.DeleteSlot(table_.BucketOfSlot(slot), slot, value);
    }

    /**
     * Clears out entries whose count has reached zero, a couple per insertion,
     * whichever case that insertion fell into.
     *
     * Those entries are dead weight: an entry at zero answers zero, which is
     * what an unmonitored key answers too, so removing it costs no accuracy and
     * frees a slot. What it buys is the **load factor**, and that is the whole
     * of this configuration's cost.
     *
     * A decrement empties every entry that was at one, which on a skewed stream
     * is most of the ones recently admitted -- hundreds or thousands at a time.
     * Leaving all of them for the arrival that needs a slot to clear is an
     * `O(w)` insertion, and the worst one the sketch makes: measured on kosarak
     * at 16 KB, a p9999 of 147 microseconds. Capping what that arrival may take
     * is worse still, in the average rather than the tail: it pins the table at
     * its 0.95 load factor, which is where a cuckoo filter's kick paths are
     * longest and where some of them fail outright and lose an entry, and the
     * same run costs 845 ms unbounded against 1541 ms in batches of 32.
     *
     * Draining a little on every insertion gets both. The table settles at the
     * occupancy its *live* entries call for, exactly as the unbounded batch
     * leaves it, and no single insertion does more than `drain_batch` evictions.
     * There are about four insertions per decrement here and rather more than
     * one emptied entry per decrement, so a budget of two keeps up comfortably;
     * the case-3 arrival still takes the one slot it needs for itself, so it
     * never has to wait for the drain to catch up.
     */
    void drain_emptied() {
        VALECounters *counters = table_.GetCounters();
        for (uint64_t taken = 0; taken < drain_batch; taken++) {
            if (smallest_count() != 0)
                return;
            evict_at(counters->MinSlot(), counters->MinValue());
        }
    }

    /**
     * The smallest count in the summary, or `no_count` if nothing is stored.
     *
     * The tree's root, which is a member of the counter array: one read. The
     * counters hold their counts plus `L`, and the root holds the smallest of
     * those, so taking `L` off it gives the smallest count -- however much of
     * `L` any individual group has had applied, which is the counter array's
     * business and not visible here.
     */
    uint64_t smallest_count() const {
        const VALECounters *counters = table_.GetCounters();
        const uint64_t value = counters->MinValue();
        return value == 0 ? no_count : value - counters->LazyDecrement();
    }

    /**
     * Rebuilds if VALE's tuning has gone stale upwards, i.e. if enough chunks
     * have spilled into tails arrays. Called after a single counter grows, so
     * it stays a comparison unless it actually fires.
     */
    void rebuild_if_vale_asks() {
        if (!vale_retuning_)
            return;
        table_.GetCounters()->MaybeRetune();
    }

    /** Case 2: puts `key` in at a count of one. */
    int32_t admit(uint64_t key, uint8_t flags) {
        return admit_with_count(key, flags, 1);
    }

    /**
     * Puts a fingerprint for `key` in, holding `count` of its occurrences.
     *
     * @returns 0, or a negative status code.
     */
    int32_t admit_with_count(uint64_t key, uint8_t flags, uint64_t count) {
        // Stored above the decrement everything else owes, so that it reads
        // back as `count` -- and so that it is, at `L + 1`, a minimum, which is
        // how an admission becomes the next eviction candidate. It goes in with
        // the entry rather than after it: a kick path that writes a zero first
        // and the real count second pays for two climbs of the min tree, and
        // the zero in between is an empty slot, which is the expensive kind.
        const int64_t pos = table_.InsertAt(key, flags,
                                            count + table_.GetCounters()->LazyDecrement());
        if (pos < 0)
            return static_cast<int32_t>(pos);
        rebuild_if_vale_asks();
        return 0;
    }
};

}   // namespace sublime
