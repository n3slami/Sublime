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
 * Here the set of monitored keys is a `FingerprintTable` -- a rank-and-select
 * quotient filter storing one variable-length fingerprint per monitored key --
 * and the counts are a `VALECounters` array laid over it slot for slot, so
 * that counter `i` belongs to the fingerprint in slot `i`.
 *
 * Keeping the counts *beside* the fingerprints rather than inside the slots is
 * what lets each side be compact on its own terms: the fingerprints shrink as
 * the filter expands, and the counters, under VALE, are only as long as the
 * counts they actually hold. The table mirrors every slot it shifts into the
 * counter array, so the two stay aligned without either knowing much about the
 * other.
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
 * Case 3 is the expensive one: it touches every counter, and it has to find
 * the smallest of them. Two things keep it cheap.
 *
 * ---------------------------------------------------------------------------
 * Chains
 * ---------------------------------------------------------------------------
 * A key's count is not one counter's but a *sum*: every fingerprint of its run
 * that matches it, of whatever length, holds a share of it. A short
 * fingerprint stands for a whole family of keys, so the count it holds belongs
 * to all of them at once, and adding every match up is what keeps a query from
 * ever falling short.
 *
 * A run is held in ascending slot value, and the void bit outweighs the
 * fingerprint, so a run is in ascending order of *length*: an earlier entry of
 * a run is a prefix of a later one exactly when the two match. The entries
 * matching a key therefore form a chain, ordered by length, and the count of
 * the family that ends at entry `e` is its **chain sum** -- its own counter
 * plus the counters of every earlier matching entry of its run.
 *
 * ---------------------------------------------------------------------------
 * The lazy decrement counter
 * ---------------------------------------------------------------------------
 * Nothing is decremented in place. A single `lazy_decrement_` stands for every
 * decrement owed, and it is owed by each *key*, once, however many entries the
 * key's count is spread over:
 *
 *      count of the key whose longest match is e  =  chain sum of e  -  L
 *
 * A key admitted with nothing already matching it is therefore stored at
 * `L + 1`, so that it reads back as one; one admitted onto a chain that
 * already carries the decrement is stored at just its own count. Case 3 then
 * costs a single increment of `L` rather than a pass over the array.
 *
 * The counters at the bottom of each chain carry that offset as dead weight,
 * though, and dead weight costs bits under VALE. So it is periodically
 * *merged*: one pass takes it off the first entry of every chain -- and only
 * that entry, since that is the one place the chain carries it -- and resets
 * it to zero. VALE is re-tuned in the same breath, the counters having just
 * got smaller. The merge asks whenever the lazy counter passes the mean of the
 * counts themselves, the point at which the stored values are more than twice
 * the size they need to be; that mean is maintained on the fly, as a running
 * sum of the stored values, rather than recomputed.
 *
 * ---------------------------------------------------------------------------
 * The buffer
 * ---------------------------------------------------------------------------
 * Finding the counts that reach zero still means a pass over the array, so
 * case 3 is not applied one insertion at a time. Its keys go into a buffer of
 * `B` entries, and when that fills, one pass collects the `B` smallest chain
 * sums and the whole batch is replayed against them: each buffered key that
 * finds no room raises the lazy counter by one, and any of those `B` whose
 * count reaches zero makes way, its slot going to the key whose decrement
 * emptied it. `B` decrements can only bring a count of more than `B` down to
 * something positive, so the `B` smallest are the only ones that can free a
 * slot, and one pass serves the entire batch. That is `O(n/B)` per insertion
 * rather than `O(n)`, and it costs no heap over the counters.
 *
 * The replay never runs out of room before it runs out of keys. Writing `a`
 * for the admissions made so far, `free = initial + died + ... - a`; if all
 * `B` candidates have died while a key still waits, then `a <= B - 1` and so
 * `free >= 1`. One replay therefore handles the whole batch, and the entries
 * the pass never looked at are swept up afterwards, when the decrement is
 * final: they are exactly the entries whose chain sums it has caught up with.
 *
 * Evicting a short fingerprint takes its counter out of the chain sums of the
 * longer entries that matched it, which would quietly cut *their* keys'
 * counts. `FingerprintTable::DeleteEntriesPreservingChainSums` puts it back,
 * by handing what was removed to the entries left at the bottom of each chain.
 *
 * Insertions sitting in the buffer have not been applied yet, so **a query
 * only reflects them after `FlushBuffer()`** -- the same convention the other
 * Sublime sketches use for their prefetch queues.
 *
 * ---------------------------------------------------------------------------
 * The query algorithm
 * ---------------------------------------------------------------------------
 * A query adds up the counters of every fingerprint in the key's run that
 * matches it -- the chain sum of its longest match -- and takes the lazy
 * decrement off once. Short fingerprints are the price of a table that has
 * expanded, and each one stands for a whole family of keys, so an entry
 * labelled with one may be holding another key's occurrences, or a mix of
 * several keys'. Adding all of the matches up rather than picking one of them
 * is what makes the answer an over-estimate: whichever entry holds the queried
 * key's own occurrences, it is certainly in the sum.
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
 * which is the size the expansion was expected to produce. `FingerprintTable`
 * decides for itself whether that expansion is a stretch within the current
 * period or the doubling that ends one; the threshold only cares what it left
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
#include <unordered_map>
#include <utility>
#include <vector>

#include "FingerprintTable.hpp"
#include "VALECounters.hpp"

namespace sublime {

/**
 * @tparam expand_on_error_inducing_insertions Whether the size function is
 * tested against the error-inducing insertions alone (the default) or against
 * every insertion. See the note on the size function at the top of this file.
 */
template <bool expand_on_error_inducing_insertions = true>
class SublimeMG {
    friend class SublimeMGTest;

public:
    using hashmode = FingerprintTable::hashmode;

    /** Signals that the key passed in has already been hashed. */
    static constexpr uint32_t flag_key_is_hash = FingerprintTable::flag_key_is_hash;

    /* Status codes. */
    static constexpr int32_t err_no_space = FingerprintTable::err_no_space;
    /** The key has no fingerprint in the table, so there is no count to touch. */
    static constexpr int32_t err_not_monitored = FingerprintTable::err_doesnt_exist;

    /** `B`, the number of case-3 insertions batched up before they are applied. */
    static constexpr uint64_t default_buffer_capacity = 64;

    /**
     * @param nslots The number of slots the fingerprint table holds. Need not
     * be a power of two. `Capacity()` is a shade under this.
     * @param key_bits The number of bits of the hash the table uses.
     * @param hash_mode The hashing mode, see `FingerprintTable::hashmode`.
     * @param seed The seed of the hash function.
     * @param growth_coefficient `r`, the growth coefficient of Stretching.
     * @param buffer_capacity `B`, how many insertions that find the summary
     * full are batched up before the eviction pass runs. Larger amortizes that
     * pass further, at the cost of holding the batch and of a coarser
     * approximation of the order the evictions would have happened in.
     * @param expansion_f The inverse of the size function `W` of the paper:
     * given a number of monitored keys, the stream length at which that many
     * stops being enough. Leave it empty to keep the summary at a fixed size,
     * which is plain Misra-Gries.
     */
    SublimeMG(uint64_t nslots, uint64_t key_bits, hashmode hash_mode, uint32_t seed,
              uint32_t growth_coefficient = 1,
              uint64_t buffer_capacity = default_buffer_capacity,
              std::function<uint64_t(double)> expansion_f = {}):
            table_{nslots, key_bits, hash_mode, seed, growth_coefficient},
            buffer_capacity_{buffer_capacity},
            expansion_f_{std::move(expansion_f)} {
        assert(buffer_capacity_ > 0);
        table_.EnableCounters();
        buffer_.reserve(buffer_capacity_);
        retarget();
    }

    /**
     * Counts one more occurrence of `key`, applying whichever of the three
     * Misra-Gries cases fits. A case-3 insertion is buffered rather than
     * applied, so it does not show up in a query until the buffer fills or
     * `FlushBuffer` is called.
     *
     * @returns 0, or a negative status code.
     */
    int32_t Insert(uint64_t key, uint8_t flags = 0) {
        // Tested before this insertion is counted, so that the measure the
        // threshold reads is complete: whether *this* key is error-inducing is
        // not known until it has been looked up, and the lookup has to happen
        // after any expansion, which moves every entry.
        if (SizeMeasure() >= expansion_lim_)
            grow_to_fit();
        n_++;

        const int64_t pos = table_.FindLongestMatch(key, flags);
        if (pos >= 0) {                             // Case 1: already monitored.
            table_.GetCounters()->Increment(pos);
            counter_sum_++;
            rebuild_if_vale_asks();
            return 0;
        }
        // Nothing in the table matched, so this occurrence is one the summary
        // could not simply count -- see the size function note up top.
        error_inducing_++;

        if (CountMonitored() < Capacity())          // Case 2: room to admit it.
            return admit(key, flags);

        // Case 3: the summary is full, so this one waits for the batch.
        buffer_.push_back({key, flags});
        if (buffer_.size() >= buffer_capacity_)
            FlushBuffer();
        return 0;
    }

    /**
     * Applies the buffered insertions. One pass collects the `B` smallest
     * chain sums, the whole batch is replayed against them, and a second pass
     * evicts every entry the decrement ended up emptying.
     *
     * @returns The number of entries evicted.
     */
    uint64_t FlushBuffer();

    /**
     * Estimates the frequency of `key`: the sum of the counts of *every*
     * stored fingerprint matching it, of whatever length.
     *
     * That sum is the chain sum of the key's longest match, less the lazy
     * decrement, which the key owes once rather than once per entry.
     *
     * A fingerprint shorter than the full length matches a whole family of
     * keys, so the entry it labels may be another key's, or may be a mix of
     * several keys' occurrences that the table could not tell apart -- but
     * whatever entry holds `key`'s own occurrences is certainly among those
     * summed. So the estimate never falls short of the count Misra-Gries
     * itself would hold, and errs by the counts of the other entries the
     * key's hash happens to match, which is why the fingerprints are kept as
     * long as the space allows.
     *
     * Buffered insertions are not reflected until `FlushBuffer`.
     *
     * @returns The estimated count of `key`, or 0 if nothing matches it.
     */
    uint64_t Query(uint64_t key, uint8_t flags = 0) const {
        const FingerprintTable::MatchSum matches =
                table_.SumMatchingCounters(key, flags);
        if (matches.count == 0)
            return 0;
        // The decrement is owed by the key, once, however many fingerprints
        // its count is spread over.
        assert(matches.sum >= lazy_decrement_);
        return matches.sum - lazy_decrement_;
    }

    /** @returns True if some stored fingerprint matches `key`. */
    bool IsMonitored(uint64_t key, uint8_t flags = 0) const {
        return table_.FindLongestMatch(key, flags) >= 0;
    }

    /** Empties the summary, keeping its shape. */
    void Reset() {
        table_.Reset();   // Resets the counters along with it.
        buffer_.clear();
        lazy_decrement_ = 0;
        counter_sum_ = 0;
        total_decrements_ = 0;
        n_ = 0;
        error_inducing_ = 0;
        retarget();
    }

    /**
     * Grows the summary, keeping every monitored key and its count, and with
     * it the number of keys it can monitor. Flushes the buffer first.
     *
     * @returns The number of fingerprints afterwards -- more than there were
     * if any void entry was duplicated -- or a negative status code.
     */
    int64_t Expand() {
        FlushBuffer();
        const int64_t res = table_.Expand();
        if (res >= 0) {
            recount();
            retarget();
        }
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
        FlushBuffer();
        const int64_t res = table_.Contract();
        if (res >= 0) {
            recount();
            retarget();
        }
        return res;
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
    /** @returns The decrement every stored counter currently owes. */
    uint64_t GetLazyDecrement() const {
        return lazy_decrement_;
    }
    /** @returns Every decrement ever applied, merged ones included. */
    uint64_t CountDecrements() const {
        return total_decrements_;
    }
    uint64_t CountBuffered() const {
        return buffer_.size();
    }
    uint64_t GetBufferCapacity() const {
        return buffer_capacity_;
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
    /** @returns How many times the size function has grown the summary. */
    uint64_t CountExpansions() const {
        return table_.GetExpansionCount();
    }
    /** @returns The bytes held by the fingerprints, the counters, and the buffer. */
    uint64_t SizeInBytes() const {
        return table_.SizeInBytes() + table_.GetCounters()->SizeInBytes()
             + buffer_.capacity() * sizeof(BufferedKey);
    }

    const FingerprintTable& Table() const {
        return table_;
    }
    const VALECounters& Counters() const {
        return *table_.GetCounters();
    }

private:
    /** An insertion waiting for the batch. Kept as the key, not its hash, so
     *  that it hashes exactly as it would have on the way in. */
    struct BufferedKey {
        uint64_t key;
        uint8_t flags;
    };

    /** A key of the batch that has taken a slot, but is not in the table yet. */
    struct Pending {
        uint64_t key;
        uint8_t flags;
        uint64_t identity;      /**< What the table would keep of its hash. */
        uint64_t count;
        uint64_t dies_at;       /**< The value of `dec` its count runs out at. */
        bool dead;
    };

    /*
     * The bare Misra-Gries primitives. Not public: each one can leave the
     * summary in a state the algorithm itself would never produce -- over
     * capacity, or holding a count the table never saw -- and `Insert` and
     * `Query` are what a caller is meant to have. They flush the buffer first,
     * so that the batch waiting in it, whose keys matched nothing when they
     * were buffered, still matches nothing when it is applied.
     */

    /**
     * Starts monitoring `key`, whether or not there is room and whether or not
     * it is already monitored: its count goes up by one, from zero if nothing
     * matched it. The bare case-2 primitive.
     *
     * @returns 0, or `err_no_space` if the table is full.
     */
    int32_t StartMonitoring(uint64_t key, uint8_t flags = 0) {
        FlushBuffer();
        return admit(key, flags);
    }

    /**
     * Stops monitoring `key`: drops the longest stored fingerprint matching it
     * along with its count, repairing the chains that ran through it. The bare
     * eviction primitive.
     *
     * @returns 0, or `err_not_monitored` if no fingerprint matches `key`.
     */
    int32_t StopMonitoring(uint64_t key, uint8_t flags = 0) {
        FlushBuffer();
        int64_t delta = 0;
        const int32_t res = table_.DeletePreservingChainSums(key, flags, delta);
        if (res < 0)
            return err_not_monitored;
        counter_sum_ = static_cast<uint64_t>(static_cast<int64_t>(counter_sum_) + delta);
        return 0;
    }

    /**
     * Subtracts the lazy decrement counter out of the first counter of every
     * chain -- the one place a chain carries it -- and resets it to zero,
     * leaving every count exactly as it reads now. Re-tunes VALE on the way
     * out, the counters having just got smaller.
     */
    void MergeLazyDecrement() {
        if (lazy_decrement_ != 0)
            rebuild();
    }

    /**
     * Merges the lazy decrement counter if it has grown past the mean of the
     * counts, which is when the stored counters are more than twice the size
     * they need to be.
     *
     * @returns True if the merge happened.
     */
    bool MaybeMergeLazyDecrement() {
        if (!lazy_decrement_is_dead_weight())
            return false;
        MergeLazyDecrement();
        return true;
    }

    FingerprintTable table_;
    std::vector<BufferedKey> buffer_;
    uint64_t buffer_capacity_;
    /** What every stored counter owes; see the note at the top of this file. */
    uint64_t lazy_decrement_ = 0;
    /** The sum of the stored counters, maintained as insertions come in. */
    uint64_t counter_sum_ = 0;
    /** Every decrement ever applied. Bounds how far a count can lag the truth. */
    uint64_t total_decrements_ = 0;
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
     * @returns True once the lazy decrement has grown into more than the
     * counts it is subtracted from, so that the stored values are more than
     * twice the size they need to be and a pass to shrink them pays off.
     */
    bool lazy_decrement_is_dead_weight() const {
        const uint64_t monitored = CountMonitored();
        if (lazy_decrement_ == 0 || monitored == 0)
            return false;
        // The mean stored value is `counter_sum_ / monitored`, of which the
        // lazy decrement is dead weight and the rest is the mean count. So the
        // dead weight has overtaken the count once it passes half of the
        // stored value. Written as a division to keep the product in range.
        //
        // Only the first entry of each chain actually carries the decrement,
        // so this over-states the dead weight once runs hold chains, and
        // merges a little sooner than it strictly has to. Counting the chains
        // would cost a pass over the table on every insertion, which is a good
        // deal more than the eagerness costs.
        return counter_sum_ / monitored < 2 * lazy_decrement_;
    }

    /**
     * Takes the lazy decrement off, if there is one, and re-tunes the counter
     * array to whatever it is left holding.
     *
     * The decrement is owed once per chain, not once per counter, so it comes
     * off the one entry of each chain that carries it -- the one with nothing
     * below it in its run that matches -- rather than off everything. That is
     * a pass of its own, over the entries alone; VALE's own pass, over every
     * counter, then follows and is told the values have shrunk, so that it is
     * free to re-tune to them however it likes.
     */
    void rebuild() {
        VALECounters *counters = table_.GetCounters();
        if (lazy_decrement_ == 0) {
            counters->Retune();
            return;
        }
        const uint64_t owed = lazy_decrement_;
        uint64_t chains = 0;
        table_.ForEachEntryWithChainSum([&](const FingerprintTable::ChainedEntry& entry) {
            if (entry.chain_sum != entry.count)
                return;     // Something below it in the run is carrying it.
            assert(entry.count > owed);
            counters->Set(entry.slot, entry.count - owed);
            chains++;
        });
        counter_sum_ -= owed * chains;
        lazy_decrement_ = 0;
        counters->Retune(0, /*shrank=*/true);
    }

    /**
     * Rebuilds if VALE's tuning has gone stale. Called after a single counter
     * changes, so it stays a comparison unless it actually fires -- the lazy
     * decrement cannot have moved, and it rides along on the rebuild anyway.
     */
    void rebuild_if_vale_asks() {
        if (table_.GetCounters()->ShouldRetune())
            rebuild();
    }

    /**
     * Rebuilds if either side wants it. Called where the lazy decrement may
     * just have grown, which is the only thing that makes the merge's own
     * trigger newly true.
     */
    void rebuild_if_either_asks() {
        if (table_.GetCounters()->ShouldRetune() || lazy_decrement_is_dead_weight())
            rebuild();
    }

    /** Case 2: puts `key` in at a count of one. */
    int32_t admit(uint64_t key, uint8_t flags) {
        return admit_with_count(key, flags, 1);
    }

    /**
     * Puts a fingerprint for `key` in, holding `count` of its occurrences.
     *
     * What that fingerprint is stored at depends on whether the key already
     * had a chain. With nothing matching it, its entry is the whole chain and
     * has to carry the lazy decrement itself, so it is stored `L + count`.
     * With something matching it -- a shorter fingerprint the table has kept
     * from before it expanded -- the decrement is already carried further down
     * that chain, and this entry holds just the `count` on top of it.
     *
     * @returns 0, or a negative status code.
     */
    int32_t admit_with_count(uint64_t key, uint8_t flags, uint64_t count) {
        const FingerprintTable::MatchSum matches = table_.SumMatchingCounters(key, flags);
        const int64_t pos = table_.InsertAt(key, flags);
        if (pos < 0)
            return static_cast<int32_t>(pos);
        const uint64_t stored = matches.count == 0 ? lazy_decrement_ + count : count;
        table_.GetCounters()->Set(pos, stored);
        counter_sum_ += stored;
        rebuild_if_vale_asks();
        return 0;
    }

    /**
     * One pass over the table for the `count` smallest chain sums. Keeps a
     * bounded max-heap rather than sorting or building a heap of everything,
     * so the pass costs no more memory than the batch it serves.
     *
     * @returns The chain sums, smallest first.
     */
    std::vector<uint64_t> smallest_chain_sums(uint64_t count) const;

    /** Recomputes what is tracked on the fly, after a resize rearranges it. */
    void recount() {
        counter_sum_ = 0;
        const VALECounters *counters = table_.GetCounters();
        for (auto it = table_.begin(); it != table_.end(); ++it)
            counter_sum_ += counters->Get(it.slot());
    }
};


template <bool expand_on_error_inducing_insertions>
inline std::vector<uint64_t>
SublimeMG<expand_on_error_inducing_insertions>::smallest_chain_sums(uint64_t count) const {
    const auto larger = [](uint64_t a, uint64_t b) { return a < b; };
    std::vector<uint64_t> heap;
    heap.reserve(count);

    table_.ForEachEntryWithChainSum([&](const FingerprintTable::ChainedEntry& entry) {
        if (heap.size() < count) {
            heap.push_back(entry.chain_sum);
            std::push_heap(heap.begin(), heap.end(), larger);
        }
        else if (entry.chain_sum < heap.front()) {
            // The heap's root is the largest of the smallest so far, so it is
            // the one this entry displaces.
            std::pop_heap(heap.begin(), heap.end(), larger);
            heap.back() = entry.chain_sum;
            std::push_heap(heap.begin(), heap.end(), larger);
        }
    });
    std::sort_heap(heap.begin(), heap.end(), larger);   // Smallest first.
    return heap;
}


template <bool expand_on_error_inducing_insertions>
inline uint64_t SublimeMG<expand_on_error_inducing_insertions>::FlushBuffer() {
    if (buffer_.empty())
        return 0;

    const uint64_t monitored = CountMonitored();
    uint64_t free_slots = monitored < Capacity() ? Capacity() - monitored : 0;
    // Each key of the batch needs at most one slot, so if there are already
    // that many going spare nothing can be decremented and the pass over the
    // table is not worth making. Otherwise: `B` decrements can only bring a
    // count of at most `B` to zero, so the `B` smallest chain sums are the
    // only entries that can free a slot, whatever order the batch applies in.
    std::vector<uint64_t> candidates;
    if (free_slots < buffer_.size())
        candidates = smallest_chain_sums(buffer_.size());

    uint64_t dec = 0;
    size_t next_candidate = 0;
    std::vector<Pending> pending;
    std::unordered_map<uint64_t, size_t> pending_at;
    // A min-heap of the pending entries by the decrement they run out at.
    // Counting one up pushes it back rather than moving it, so a record that
    // no longer agrees with its entry is stale and skipped.
    std::vector<std::pair<uint64_t, size_t>> deaths;
    const auto later = [](const std::pair<uint64_t, size_t>& a,
                          const std::pair<uint64_t, size_t>& b) { return a.first > b.first; };

    // Everything the decrement has caught up with makes way, whether it is an
    // entry of the table or a key of this very batch waiting to go in.
    const auto reap = [&]() {
        while (next_candidate < candidates.size()
                && candidates[next_candidate] <= lazy_decrement_ + dec) {
            next_candidate++;
            free_slots++;
        }
        while (!deaths.empty() && deaths.front().first <= dec) {
            std::pop_heap(deaths.begin(), deaths.end(), later);
            const auto [dies_at, index] = deaths.back();
            deaths.pop_back();
            if (pending[index].dead || pending[index].dies_at != dies_at)
                continue;
            pending[index].dead = true;
            free_slots++;
        }
    };

    const auto start_pending = [&](const BufferedKey& buffered, uint64_t identity) {
        pending.push_back({buffered.key, buffered.flags, identity, 1, dec + 1, false});
        pending_at[identity] = pending.size() - 1;
        deaths.push_back({dec + 1, pending.size() - 1});
        std::push_heap(deaths.begin(), deaths.end(), later);
        free_slots--;
    };

    // Work the batch out without touching the table: evicting an entry slides
    // its neighbours along, which would move the very slots this is naming.
    for (const BufferedKey& buffered : buffer_) {
        const uint64_t identity = table_.EntryIdentity(buffered.key, buffered.flags);
        const auto at = pending_at.find(identity);
        if (at != pending_at.end() && !pending[at->second].dead) {
            // Two keys of the batch the table cannot tell apart. They are the
            // same key as far as the summary goes, so the second counts the
            // first up -- and buys it one more decrement of life.
            Pending& already = pending[at->second];
            already.count++;
            already.dies_at++;
            deaths.push_back({already.dies_at, at->second});
            std::push_heap(deaths.begin(), deaths.end(), later);
            continue;
        }
        if (free_slots > 0) {
            start_pending(buffered, identity);
            continue;
        }
        // The Misra-Gries decrement: every count comes down by one, and any
        // that reach zero make way. The occurrence that paid for it takes one
        // of the slots it freed; if it freed none, the occurrence is lost.
        dec++;
        reap();
        if (free_slots > 0)
            start_pending(buffered, identity);
    }

    lazy_decrement_ += dec;
    total_decrements_ += dec;

    // With the decrement settled, the entries it emptied are exactly those
    // whose chain sums it has caught up with -- the candidates that ran out
    // above, and any entry sitting just as low that the pass never looked at.
    uint64_t evicted = 0;
    if (dec > 0) {
        std::vector<std::pair<uint64_t, uint64_t>> evictions;
        table_.ForEachEntryWithChainSum([&](const FingerprintTable::ChainedEntry& entry) {
            if (entry.chain_sum <= lazy_decrement_)
                evictions.push_back({entry.bucket, entry.slot});
        });
        evicted = evictions.size();
        const int64_t delta = table_.DeleteEntriesPreservingChainSums(evictions);
        counter_sum_ = static_cast<uint64_t>(static_cast<int64_t>(counter_sum_) + delta);
    }

    // Then the keys that made it, each holding the occurrences of the batch
    // that landed on it. None of them matched anything when it was buffered,
    // and the flush has only taken entries away since, so each one goes in as
    // a chain of its own.
    for (const Pending& admitted : pending) {
        if (admitted.dead)
            continue;
        admit_with_count(admitted.key, admitted.flags, admitted.count);
    }

    buffer_.clear();
    // The lazy decrement has just moved, so this is where its own trigger can
    // newly come due.
    rebuild_if_either_asks();
    return evicted;
}

}   // namespace sublime
