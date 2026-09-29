# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Sublime (repo dir name: `Sketchbook`) is the artifact for the PACMMOD paper *"Sublime:
Sublinear Error & Space for Unbounded Skewed Streams"*. It is a **header-only C++17
framework** that generalizes frequency-estimation sketches (Count-Min, Count Sketch) so they
adapt to workload skew (short counters that elongate on overflow, within one cache line) and
to stream length (expanding/contracting the counter array). Everything else in the repo —
`bench/`, `tests/`, `examples/` — exists to evaluate and validate `include/`.

Two experiments post-date the conference paper and have no figure number in the scripts' help
text: the ℓ2-norm size function for `SublimeCS` (`l2_size_function`) and TPC-H join-size
estimation (`join_size`). Their plotters do emit `Fig_15`/`Fig_16`.

## Build & Test

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release   # -DBUILD_TESTS=0 / -DBUILD_EXAMPLES=0 / -DBUILD_BENCHMARKS=0
make -j8
```

**On CMake 4.x add `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`.** Tests fetch doctest 2.4.11, whose
own `CMakeLists.txt` declares `cmake_minimum_required(VERSION 3.0)`; CMake 4 refuses that and
the configure fails inside `_deps/doctest-src`, not in any file of this repo. Only bites when
`BUILD_TESTS` is on.

- Release adds `-march=native`; every sketch library is compiled with `-Ofast -march=native`
  (`BITHACKING_COMPILE_FLAGS`). The code relies on BMI2/AVX-512 intrinsics (`_pdep_u64`,
  `_mm512_*` in `SublimeCS`'s AMS estimator), so it will not run on machines lacking them.
- `-DBUILD_BENCHMARKS=1` (default) runs `bench/scripts/setup_includes.sh` at configure time,
  which inits the `waving_sketch` and `salsa` submodules and **`sed`-patches**
  `waving_sketch/include/Waving.h` to add `#include <unistd.h>`. The patch is not idempotent —
  the submodule accumulates a duplicate include line on every fresh configure, which is why it
  shows as dirty in `git status`. Don't try to "fix" that dirt by committing it; reset the
  submodule instead.

Tests use doctest (fetched by CMake) and are run from the **build directory** (`enable_testing()`
lives in the top-level `CMakeLists.txt`, so `CTestTestfile.cmake` is generated there):

```bash
ctest -VV                 # all suites
ctest -R sublime_cms      # one suite: cms | sublime_cms | cs | sublime_cs | cuckoo_table |
                          #            vale_counters | sublime_mg | mg | space_saving | all
./tests/SublimeCMSTests --test-case="monte carlo"        # one doctest case
```

`ctest` builds Release, which defines `NDEBUG` — so every `assert` in the headers is compiled
out and the suites never check the internal invariants (`bits_per_slot_ <= 56` and
`fingerprint_bits_ >= 1` in `CuckooTable::allocate`, the agreement between
`DecrementIsZero`'s fast test and `Get(pos) == 0` in `VALECounters`, ...). Worth a
separate assert-enabled build when adding tests, since an invalid configuration otherwise
passes quietly:

```bash
g++ -std=c++17 -O2 -g -march=x86-64-v3 -mbmi2 -Iinclude -Ibuild/include \
    -Ibuild/_deps/doctest-src tests/SublimeMGTests.cpp -o /tmp/mg_assert && /tmp/mg_assert
```

That `-march` is also what makes the binary runnable under valgrind: a `-march=native` build
here emits AVX-512 that valgrind cannot decode and dies with SIGILL.

Test files declare a `SublimeCMSTest` / `SublimeCSTest` class that is a `friend` of the sketch,
so they poke at private internals (`set_counter`, `sketches.back()`, `stub_size`) directly;
`CuckooTableTest`, `VALECountersTest`, and `SublimeMGTest` do the same. Add
new white-box checks as static methods on that friend class and wire them into a `TEST_CASE`.

## Sublime_MG (journal extension)

The `journal_extension` branch adds Sublime_MG — Sublime applied to Misra-Gries. It is
`CuckooTable` (the monitored-key set), `VALECounters` (the counts), and the wiring between
them in `SublimeMG`. The table mirrors every slot it moves into the counter array it owns
(`EnableCounters`), which is also what makes the counter prefetch in `FindMatch` possible. The
algorithm, its tests, both benchmark experiments, and their figures are all done (see "State of
play").

**There used to be a second table**, a rank-and-select quotient filter called
`FingerprintTable`, and it is *gone* — deleted, not deprecated, along with its 24-case suite.
Everything below describes the cuckoo table. `SublimeMG` and `MG` still take their table as a
template parameter, so another one can be dropped in, but there is one in the tree and it is
the default. Where the RSQF still gets a mention it is to explain why something is the way it
is; `git log` has it if it is ever wanted back.

The table *owns* its counters (`EnableCounters`, not an attach-a-pointer arrangement), because
the two must be resized in lockstep and only the table knows when that happens. `Expand` and
`Contract` carry every count across with its fingerprint, and neither ever duplicates or drops
an entry: a resize is a pure re-hash.

### Fixed-length fingerprints — the one idea the rest follows from

**Every fingerprint in the table is of the same length**, so a key corresponds to exactly one
entry and its count is that entry's counter. `key_bits` is fixed for the life of the table;
what a period-ending expansion does is widen the bucket index by one bit and shorten *every*
fingerprint — stored and freshly inserted alike — by one. `bucket + fingerprint` therefore
always covers exactly `key_bits` bits of hash, however often the table has been resized.

Three things fall out of that, and they are why the code is as small as it is:

- **A slot holds a fingerprint and one flag bit**, the flag saying whether the entry is in its
  primary bucket or its alternate, which is what makes the whole hash recoverable (see the
  `CuckooTable.hpp` row). A slot gets one bit *narrower* per period: a table that has grown is
  cheaper per slot than it was. Matching is plain equality, over the two buckets a key can be in.
  Note what `key_bits` has to be for a fingerprint of a wanted length: the bucket index is
  `log2(nslots / base_depth)`, so `CuckooTable::KeyBitsFor` is the thing to ask, and `MG` and
  both benches do. Adding `log2(nslots)` by hand, as they did while the quotient filter was
  here, quietly stores fingerprints two bits longer than asked for and pays for them per slot.
- **A resize is a pure re-hash.** `const_iterator::hash()` is lossless, so `rebuild_into` just
  hands each entry's hash to the destination, which splits it by its own shape. That one line
  serves a deepening, a doubling and a contraction alike, and contraction restores the same
  entries with the same counts — though not the same *slots*, since re-placing them kicks.
- **Growth is bounded by the fingerprint, not by the hash.** `Expand` refuses once the length
  is down to 1 (a zero-width slot cannot tell keys apart, and the packing cannot represent it),
  and `CountSlotsAfterExpansion` says so beforehand.

Shortening is what a resize costs in accuracy: a shorter fingerprint stands for a larger family
of keys, so entries the table could once tell apart merge into one, which only ever
over-estimates. `Query(k)` is the counter of `k`'s matching entry, or 0.

### The table, and two decrements

`SublimeMG<expand_on_error_inducing_insertions, use_min_tree, Table>` and `MG<Table>` take the
monitored-key set as a **template parameter**, defaulting to `CuckooTable` — the only one there
is. Neither sketch knows anything about it beyond the ~20 methods it calls.

The cuckoo table replaced the RSQF because the RSQF's cost is *shifting*: an insert or delete
slides a whole cluster, which at a 0.95 load factor is hundreds of slots, each dragging its
counter and the min tree's repair with it (the tree was inside the counter array then). A cuckoo
filter relocates one entry per kick and
never shifts.
Measured on kosarak at 16 KB while both still existed, insert latency: RSQF sweep 794 ms, RSQF +
tree 5220 ms, **cuckoo 451 ms, cuckoo + tree 1132 ms** — the tree configuration costs 4.6x less on the cuckoo table,
which is the whole reason the two changes landed together. It also monitored ~17% more keys
per byte in both configurations, a cuckoo slot carrying none of the RSQF's 2.1 bits of block
metadata.

**Where the current numbers stand**, kosarak at a 16 KB budget, seed 12345, after the removal
(which also fixed the fingerprint sizing — see `KeyBitsFor` above — so the slots are two bits
narrower than in the comparison above and every configuration improved):

| | AAE | size [B] | capacity | insert |
|---|---|---|---|---|
| `MG` | 112.9 | 14568 | 1702 | 364 ms |
| `SublimeMG` (sweep) | 91.8 | 17892 | 2918 | 388 ms |
| `SublimeMG` (min tree) | 100.0 | 16104 | 2432 | 632 ms |

The min tree row is after the 2026-09-28 rewrite; it was 19016 bytes and 1096 ms before, holding
the tree inside a doubled counter array.

The sizes run over the budget because the budget search probes a *fresh* sketch and VALE's
counters grow with the counts; the plots are (actual size, error) curves, so it only means the
points sit a little to the right. See the `CuckooTable.hpp` row in the architecture table.

**Both buckets are prefetched before either is read.** A lookup's two candidate buckets are
known from the hash, and reading one and then the other puts two independent cache misses in
series; prefetching both slot regions up front is worth **5-7% of insert and query time** once
the table outgrows the last-level cache (1M slots: `MG` insert 723 -> 677 ms, Sublime_MG sweep
797 -> 744, tree 1581 -> 1509; queries 617 -> 593, 878 -> 813, 845 -> 816), and is inside the
noise at 64k slots where everything fits. Two things were tried and are *not* in the code, with
the measurements in the headers: prefetching **both** buckets' counters (slightly worse than
prefetching neither — only one bucket can hold the entry, so one of the two is always wasted),
and the same trick for the RSQF's own blocks (no effect, because a slot's address does not exist
until the block metadata has arrived, so there is no second miss to overlap).

**The table can lose an entry, which the quotient filter never did.** When a kick path runs out of
patience (`max_kicks`) the entry it is carrying has nowhere to go: if that is the arrival,
`InsertAt` returns `err_no_space` and the occurrence is dropped, and if it is an older tenant
the arrival is stored and the tenant is dropped, count and all, which `CountLostEntries()`
reports. Both happen at the 0.95 load factor Misra-Gries runs the table at — a few hundred over
a 300k-insertion stream — and both are *silent* accuracy loss, so the bench's extras carry the
count. It is also why the cuckoo test cases cannot be held to an exact oracle: a loss leaves the
summary with room the oracle does not have, so the two decrement at different times and diverge
from there. `MGTest::CuckooMonteCarlo` follows the oracle in lockstep up to the first loss and
falls back to the invariants no loss can break, and
`SublimeMGTest::CheckMisraGriesGuarantee` drops the understatement half of the guarantee once
`LostEntries(mg) > 0`, keeping the over-estimation half, which losing counts cannot violate.

`use_min_tree` is the **second template parameter**, default `false`. It picks between two ways
of applying the Misra-Gries decrement, and every place they differ is an `if constexpr`.
`bench_SublimeMG --min-tree` selects the tree at run time, the way `--expand-measure` selects
the measure.

### Insertion: the decrement sweep (`use_min_tree = false`, the default)

- **Only `Insert`, `Query`, `Reset`, `Expand`/`Contract` and the read-only accessors are
  public.** `StartMonitoring`/`StopMonitoring` are private bare primitives (the tests are a
  `friend`): each can leave the summary in a state the algorithm would never produce.
- **There is no buffer and no lazy decrement.** Case 3 — an unmonitored key arriving at a full
  summary — is applied on the spot: one sweep of the entries takes one off every count, and the
  entries it empties are evicted. The occurrence that paid for the sweep takes one of the slots
  it freed; if it freed none, the occurrence is dropped. `CountDecrements()` counts *sweeps*,
  which is the classic Misra-Gries parameter: no key's count is understated by more than that.
- **An entry is evicted the moment the sweep empties it.** A deletion clears one slot and moves
  nothing else, and the iterator has already passed that slot, so there is nothing to defer.
  (Over the RSQF this had to collect its victims and delete them after the pass, because
  removing an entry slid the rest of its cluster down over the hole and moved slots the sweep
  had yet to reach. That is also why `DeleteSlots` no longer exists.)
- **`VALECounters::DecrementIsZero` is why the sweep is affordable.** A VALE counter holds zero
  exactly when its overflow bit is clear and its stub reads zero, so finding the entries to
  evict costs nothing beyond the decrement that was happening anyway — no extension, and no
  tails array, is ever decoded for it. This is the one place the eager decrement is *better*
  than the lazy one it replaced.
- **VALE is tuned from the same histogram `SublimeCMS` uses**, in both directions. Upwards:
  `MaybeRetune` after a counter grows, the tails-fraction trigger, exactly as in `SublimeCMS`.
  Downwards: `RetuneIfNarrower`, because counts here *shrink* and shrinking never spills a
  chunk. That one costs a pass of its own, so the sweep asks for it only every
  `ShrinkRetuneInterval()` (`2^(stub-1)`) sweeps — the point at which every counter can have
  lost a whole bit.

### Insertion: the min segment tree (`use_min_tree = true`)

The sweep is `O(w)` in the monitored keys. The tree replaces it with a **lazy decrement `L`**
and an eviction that costs `O(log w)`: a decrement is `L++`, and a key's count is its counter
less `L`, so a count reaches zero exactly when the smallest counter catches up with `L`.

This was **redesigned from the ground up** on 2026-09-28, and the old shape is worth knowing only
so as not to reintroduce it: the tree used to live *inside* the counter array, which doubled it
(`2n` counters for `n` slots, leaves in `[n, 2n)`), with each internal node holding its subtree's
minimum outright at full width, plus a per-leaf "not my pair's minimum" bit, plus a global
`L`-merging pass run `merge_batch` counters at a time that left the array on two scales and made
"is any count zero" a pair of `MinInRange` queries. All of that is gone. `git log` has it.

- **The counters are the classic `n`.** The tree is a separate packed bit array and **an internal
  node stores the position of its subtree's minimum, as an offset within that subtree** -- one
  bit at the bottom level, two the level above, four above that, each rounded up to a power of
  two so that no field straddles a 64-bit word. That is 2.29 bits a counter, against the whole
  extra counter (12-14 bits) the old shape cost. Level `l` holds `ceil(n / 2^l)` nodes, node `j`
  covering leaves `[j * 2^l, (j+1) * 2^l)`; a non-power-of-two `n` needs **no padding**, only a
  bounds check per level for the last node of a level, which can be short.
- **The root is not in the array.** It is `min_pos_` and `min_value_`, two members, so `MinSlot`
  and `MinValue` are one read each and **nothing ever descends from the root** -- where a tree of
  minima had to walk back down to turn a value into a slot, and spent 54 cycles an insertion
  doing it.
- **`L` is applied to the counters a group of 32 at a time, on demand.** `group_applied_[g]` is
  how much of `L` group `g` has had subtracted; what it still owes is `L - group_applied_[g]`.
  `MaybeFlushGroup(pos)` subtracts the difference from one group's 32 counters when a caller
  touches it and it has fallen more than `1 << (stub_size - 2)` behind, which Sublime_MG does on
  every case-3 insertion, at the group the arriving key hashes to. One `uint32` per 32 counters
  is **one bit a counter**. Two things make it cheap, and both come from a group being **an
  aligned block of 32 slots, which is exactly a level-5 subtree**:
  - **A flush changes no count, so it changes nothing in the tree.** Not "only the nodes above
    the group": nothing. Every counter of the group comes down by the same amount and its
    `group_applied_` entry goes up by it, so every *value* is exactly what it was, and the tree
    compares values. A flush is a re-encoding of one or two cache lines.
  - **Comparisons inside a group need no rebasing**, since both sides owe the same amount, so
    the bottom five levels of a climb compare stored forms directly.
  A flush always holds **one** unit back, so that a count of zero does not store the zero that
  means *empty*. That is the same rule the old design's "merge out `L - 1`, not `L`" was.
- **Three scales, and only the middle one is public.** A counter has a *count*, a *value* (the
  count plus `L`, which is what `Get`, `Set`, `MinValue` and everything else here speak), and a
  *stored form* (the value less what its group has had applied), which never leaves the class.
  `L` itself lives in `VALECounters`, so `GetRebased`/`SetRebased` are gone: a cuckoo kick moving
  a count between slots in different groups is a plain `Set`, and the array keeps the books.
  `IncrementLazy` moves **no memory at all** -- the values are the counts plus `L`, so they are
  where they were, and neither the tree nor the minimum cache is affected.
- **A retune needs no rebuild.** It re-encodes values it does not change, and the tree is a
  function of the values alone -- it names positions, and positions do not move. Only a **resize**
  invalidates it, because that renumbers every slot; `SuspendTree`/`RebuildTree` are still there
  for that, and the rebuild is one bottom-up pass carrying each level's winners forward in a
  scratch buffer, so it decodes each counter once rather than twice.
- **The climbs split three ways**, and the currency is still counter decodes:
  - **An increment** can only push its leaf up, so a node that does not name that leaf has a
    minimum strictly below what the leaf held and cannot move. Level 1 answers that with a
    *one-bit field read and no decode at all*, which is where two increments in three stop --
    the same fast case the old per-leaf bitmap bought, for free. Above it, a node that does name
    the leaf compares against the other child for *inequality* only, because the node naming this
    leaf already says it was the smaller: a differing stub proves they differ, which proves the
    leaf was strictly below, which means one more still fits (`provably_differs`).
  - **A value that fell** -- every write below what was there, every admission into an empty slot
    -- pulls minima down and cannot push any up, so no sibling is read. And when the new value is
    at or below `MinValue()`, which is where nearly every admission lands (`L + 1` is the least a
    live counter can be), *every* node from the leaf to the root names it and nothing has to be
    compared: the climb decodes nothing and its writes do not even depend on each other.
  - **Anything else** -- a clear, a write above what was there -- is `climb`, and two facts keep
    it cheap. **Only the nodes naming the position have anything to recompute**, so the climb ends
    at the first that does not, having read one field to find out. And **once the replacement is
    level with what the position held, no sibling above can beat it**: a node naming that position
    says every leaf beneath it is at or above what it held, so the folding stops and the rest of
    the climb is a field read and a field write per level.
- **That second fact is what makes an eviction affordable**, and evictions are what this
  configuration does most. Misra-Gries evicts the smallest counter, so every eviction clears a
  position all of its ancestors name -- a climb to the root, and without the shortcut a decoded
  counter at every level of it. But a decrement empties *every* entry that was at one, which on a
  skewed stream is hundreds at a time, so the replacement is almost always level with what was
  cleared and almost always found in the first level or two. Measured on kosarak at 16 KB it took
  the tree from **10.7 decoded counters per eviction to 1.3**, and the whole configuration from
  834 ms to 660. A further split on whether the cleared position is the one the *root* names
  (`climb_from_minimum`) drops the field reads too -- there is nowhere to stop, so nothing is read
  to decide where -- for another 2%.
- **The tail was the eviction batch, and the fix was to stop batching at all.** Entries at a count
  of zero are dead weight: they answer zero, which is what an unmonitored key answers, so removing
  one costs no accuracy and frees a slot. What it buys is the **load factor**. Leaving all of them
  for the arrival that needs a slot is an `O(w)` insertion and the worst one the sketch makes
  (p9999 147 microseconds); *capping* what that arrival may take is worse still in the average,
  because it pins the table at its 0.95 load factor where a cuckoo filter's kick paths are longest
  and some fail outright -- 845 ms unbounded against 1541 ms in batches of 32. `drain_emptied`
  gets both: **`drain_batch = 2` evictions on the way out of every insertion**, whichever case it
  fell into, so the table settles at the occupancy its live entries call for and no insertion does
  more than two evictions. Budgets of 1, 2, 4 and 16 all measure the same (846-860 ms), so the
  constant is not a knob to tune; the case-3 arrival still takes the one slot it needs itself, so
  it never waits for the drain.
- **`CuckooTable::BucketOfSlot` is `slot / depth`**, which is what turns the tree's answer back
  into something the table can delete. `DeleteSlot` has an overload taking the count, because a
  caller that got the slot *from* the tree has `MinValue()` in hand and hands it over rather than
  making the array decode it again.
- **An entry travels the kick path with its count.** `InsertAt` takes an `initial_count`, so an
  admission that has to kick does not write a zero first and the real count second: two climbs,
  the first of them the expensive kind, because the zero in between is an empty slot. And each
  kick step is a **fused store-and-move** -- one `Set(slot, arriving, displaced)`, one climb, with
  the displaced count handed back in the same call.
- **`L` stays small on these workloads, which is worth knowing before optimizing the flush.** It
  only rises when nothing is already at zero, and the drain keeps zeros cleared, so a decrement is
  rare: on kosarak at 16 KB, `L` reaches **1697** over 8M insertions and the whole run makes 644
  group flushes. The flush machinery is not on the hot path there. What *is*: 12.1M counter
  decodes over the run, against the sweep's 1.34M.
- **Measure it interleaved.** This machine's clock drifts about 10% between sessions: the same
  binary measured 834 ms one hour and 915 ms the next. Build both variants, alternate them in one
  script, and take medians -- comparing against a number from an earlier run has sent me chasing
  a regression that was not there, and would equally have hidden a real one.
- **What it costs.** The tree and its group table are **3.29 bits a counter**, about 7.5% of a
  slot, so at a fixed budget the summary monitors slightly fewer keys. Whether it actually does is
  a matter of granularity: the cuckoo table's achievable slot counts go 2048, 2560, 3072, 3584,
  4096 and then double, so 7.5% either costs a whole step or nothing. Under the *auto-tuning*
  build it costs a step at every budget (capacity ratio exactly 5/6); under the **baked tunings the
  figures use it costs nothing at 13 of the 17 points**, the two configurations landing on the same
  slot count -- and where they do, their AAE and top-k AAE agree to six digits. **At equal capacity
  the tree and the sweep answer identically**, which is the cleanest statement of what the tree
  costs in accuracy: nothing. The four points where they differ (webdocs 128/256 KB, caida 1-4 MB)
  are the ones where it lost a step.
- **Which statistic sees the sweep's `O(w)` pass** is the thing to get right before claiming
  anything about tails, and `p9999` mostly does not. The sweep only decrements when nothing is
  already at zero, and it evicts everything a decrement empties, so passes are *rare*: measured on
  the committed runs, 1 insertion in 4,725 on kosarak at 16 KB, and between 1 in 29,501 and 1 in
  12.5M at all sixteen other points. A pass that happens once in 30,000 insertions sits at the
  99.9966th percentile -- above `p9999`, which therefore reports ordinary insertions, where the
  tree is 1.5-2.5x *worse* because its ordinary insertion costs more. The one point where `p9999`
  does see a pass is kosarak at 16 KB, and there the tree is **11.4x better** (2,687 ns against
  30,719).
- **What the tree buys shows in `max`, and it is not noise there**, even though max is a poor
  statistic in general (see the note under "Fig. 19"): the sweep's worst insertion **scales with the
  summary** and the tree's does not. On webdocs the sweep's max runs 1.42, 0.64, 1.31, 2.32, 4.35,
  **10.98 ms** across the six budgets while the tree's stays at 275-295 microseconds; on caida it
  runs 1.27 ... **8.32 ms** against the tree's 133-324. That is 39x at webdocs 4 MB and 43x at
  caida 4 MB, and it is `O(w)` against `O(log w)` drawn in six points on two datasets rather than
  one measurement that might have been a scheduler hiccup.


### When it grows: the size function

The constructor's last argument, `expansion_f`, is the *inverse* of the paper's size function
`W` — the same convention `SublimeCMS`/`SublimeCS` use — mapping a number of monitored keys to
the stream length at which that many stops being enough. Storing the inverse is what makes the
per-insertion test a comparison instead of an evaluation of `W`:

    expansion_lim_ = expansion_f(Capacity())

`Insert` expands when the measure reaches that threshold, which is exactly "expand once `W`
outgrows the table". The argument is `Capacity()` — slots discounted by `max_load_factor`, the
keys the summary can actually monitor — so the over-provisioning the hash table keeps for its
runs does not count as usable size. After a resize the threshold is recomputed from the new
`Capacity()`, i.e. from the size that resize was expected to produce (read `SublimeCMS::expand`,
whose `expansion_f(2 * col_count)` runs *before* `col_count *= 2`, and it is the same statement
from the other side).

**What gets counted is the template parameter** `SublimeMG<expand_on_error_inducing_insertions>`
(default `true`; write `SublimeMG<>` / `SublimeMG<false>` at call sites, as `SublimeCS` is
written). Not every insertion costs the summary accuracy: one whose key is already monitored
just increments a counter that was already there, exactly as an exact counter would, and leaves
every estimate as good as it was. It is the insertion matching *nothing* that costs — it either
adds an entry whose fingerprint the other keys must now share, or finds the summary full and
makes something get decremented. So `error_inducing_` counts the insertions that matched
nothing, `n_` counts them all, and `SizeMeasure()` returns whichever the parameter selects;
`GetStreamLength()` always reports the true `N`.

The measure only means anything relative to an `expansion_f` written for it. On a skewed stream
the two are far apart — a heavy hitter misses once and then rides for free — so the same `W`
reads very differently: 200k insertions of a `u³`-skewed stream over 40k keys give a measure of
84k against 200k, 7 expansions against 8, and 151 KB against 296 KB for comparable accuracy.
The default also settles a case the total gets wrong: a summary that never fills answers
exactly and has nothing to gain from expanding, and under the default it *cannot* expand,
because the `grow_to_fit` cap sits at one slot per error-inducing insertion and it already has
one.

The threshold is read at the top of `Insert`, before that insertion is counted — whether the
key is error-inducing is not known until it has been looked up, and the lookup has to happen
after any expansion, which moves every entry. So an expansion lands on the insertion *after*
the measure reached the threshold, which is also what `SublimeCMS` does.

- `CuckooTable::CountSlotsAfterExpansion` / `CountSlotsAfterContraction` predict a resize
  from the shape arithmetic alone, without building the table. `SublimeMG` uses the first to
  retire the threshold when the table's fingerprints are down to their last bit, so a summary
  that cannot grow stops testing rather than failing an expansion per insertion.
- Whether an expansion is a deepening inside the period or the doubling that ends it is
  `CuckooTable`'s business; the policy only reads what it left behind.
- The catch-up loop is **capped at one slot per unit of the measure** (`Capacity() <
  SizeMeasure()`), which is where Misra-Gries degenerates into exact counting and no size
  function can want more. Without that cap, termination would rest on the caller's
  `expansion_f` eventually rising above the measure, and a stale threshold — a `retarget()`
  missing after a resize, say — will double the table until the machine runs out of memory.
  It has, once.
- Misra-Gries has no deletions, so `N` never falls and nothing contracts on its own. `Contract`
  stays a manual operation and recomputes the threshold like an expansion does. Passing no
  `expansion_f` fixes the summary at its initial size, which is plain Misra-Gries.

### Public API and shape constraints

`SublimeMG<>` does **not** share the other Sublime variants' interface. Keys are `uint64_t`,
not `(const char *, length)`:

| | Sublime_MG | other Sublime variants |
|---|---|---|
| update | `Insert(uint64_t key, uint8_t flags = 0)` | `Insert(const char *, uint32_t)` |
| query | `Query(uint64_t key, uint8_t flags = 0)` | `Query(const char *, uint32_t)` |
| make pending updates visible | *(none — an insertion is applied at once)* | `FlushPrefetchQueue()` |
| space | `SizeInBytes()` | `Size(bool include_all_sketches)` |
| delete | *(none — Misra-Gries has no deletions)* | `Delete(...)` |

Constructor: `SublimeMG<E>(nslots, key_bits, hash_mode, seed, growth_coefficient = 1,
expansion_f = {})`. `Capacity()` is `nslots * max_load_factor` (0.95).
`flags` takes `flag_key_is_hash` when the caller has already hashed the key, so a string
workload has to be hashed to a `uint64_t` first. `hashmode` is `Default` (Murmur),
`Invertible` (hash as wide as the key), or `None` (key used as its own hash — skew in the
input becomes skew in the load). `Insert` returns 0 or a negative status (`err_no_space`,
`err_not_monitored`). Copy and move both work, unlike `MG`/`SpaceSaving`, which delete them
(their sub-objects hold back-pointers). `Reset`
empties the summary but **keeps the size it grew to**, and re-derives the threshold from it.

Two shape constraints, both `assert`-only and therefore **silent in the Release/NDEBUG build**.
Both are about the fingerprint, which is `key_bits` less the bucket index — and the bucket index
is `log2(nslots / base_depth)`, *not* `log2(nslots)`, because a bucket holds `base_depth` slots.
So a 256-slot table with the default depth of four indexes 64 buckets and spends 6 bits, and a
fingerprint of `key_bits - 6`:

- `key_bits - log2(bucket_count) <= 56` — a slot has to be readable by one 64-bit word — i.e.
  the fingerprint is at most 56 bits. Overshooting it passes `ctest` and trips in the
  assert-enabled build; that is exactly how the one bad test configuration was caught.
- `key_bits - log2(bucket_count) >= 1` — the fingerprint always keeps at least one bit. Since a
  period-ending expansion spends one, growth stops once the length reaches 1, and
  `CountSlotsAfterExpansion() == CountSlots()` is how the summary notices and retires its
  threshold. Note `key_bits` itself never moves.

One more, which is *not* a constraint any more but used to be a trap: the growth coefficient `r`
has to divide the bucket depth, since a period adds `base_depth` slots per bucket in `r` equal
steps. Rather than asserting that, `CuckooTable` **derives** the base depth from `r` — the
smallest multiple of `r` that is at least four — so `r` of 1, 2 or 4 gives buckets of four, `r`
of 3 gives buckets of six, and every `r` is usable.

### Benchmarking Sublime_MG

The glue and two experiments are implemented: `bench/sketches_benchmark/bench_SublimeMG.cpp`,
`bench_MG.cpp`, `bench_SpaceSaving.cpp`, all in `bench/CMakeLists.txt`'s `Targets` list and
`compile_bench` branches. **`MGDummy` was removed** in favour of the two fair baselines below.

**Baselines** (`mg_accuracy`, Fig. 17): `MG` — textbook Misra-Gries, whose monitored set is a
fixed-size `CuckooTable` (32-bit fingerprints by default) with a plain 32-bit counter array, and
whose decrement is the same sweep Sublime_MG does, with no heap or bucket list anywhere; and
`SpaceSaving` — a textbook Stream-Summary storing full 64-bit keys, deliberately pointer-heavy.
Plus `Waving`. All fixed-size. See the `MG` / `SpaceSaving` rows in the architecture table.
The figure plots Sublime_MG twice, `SublimeMG` and `SublimeMG_tree`, which is the sweep against
the min tree; `run_benchmarks.py`'s `mg_accuracy_bench` names the configurations and the `label`
argument is what keeps two runs of one binary in separate files. **Both of those lines come from
`bench_SublimeMGNoTuning`**, with VALE's tuning baked in per data point from the table in
`mg_accuracy_bench` — the same arrangement the Sublime_CMS and Sublime_CS accuracy figures use, so
the latency panels are not paying for a histogram pass and the `.tex` VALE table reports the
tuning the run was compiled for. Each configuration gets its own pair, since the tree's counters
carry whatever of the lazy decrement their group still owes and so run a notch wider. A baked tuning also makes the *size*
honest: the budget search probes an array that cannot grow into a wider one, so the points land at
or under the budget instead of overshooting it as the auto-tuned build does (kosarak at 16 KB:
15048 bytes for 2432 keys, against 17892 for 2918 — the same 0.16 keys per byte, further left on
the x-axis). **`MG` and `SublimeMG` are the same algorithm over the same monitored set**
at a fixed size — same table, same fingerprint length, same seed — so their answers agree
key for key, and the only difference is VALE against a `uint32_t` array. The `sublime_mg` suite
asserts exactly that ("agrees with plain Misra-Gries exactly"), which makes the accuracy figure
a clean read of what the counters alone buy. Both figures run at the sketches' default
fingerprint length, 32 bits (`MG::default_fingerprint_length`, which `bench_SublimeMG`'s own
default matches); `--seed` is fixed for both. Note what the length costs: a wider slot roughly
halves capacity per byte (measured over the quotient filter: Sublime_MG at 128 KB monitored 21.7k
keys against 45.7k at 10 bits, and its capacity lead over `MG` fell from ~6x to ~3.3x), which on
these datasets outweighs the collision over-estimation the longer fingerprint removes --
Sublime_MG's AAE is worse at 32 bits than at 10 nearly everywhere, though still well ahead of
every baseline.

The **`CuckooTable::SlotMirror`** hook (`AttachMirror`) is what lets `MG` keep its plain counter
array aligned as the table moves entries: the table calls the sidecar beside its own `counters_`
wherever it places, relocates, carries or clears an entry, and in `Reset`. None of this touches
Sublime_MG's path (it sets no sidecar).

Glue rules that bite (all handled in the committed benches):

- **`query_sketch` does not flush anything.** It used to: case-3 insertions were buffered, and a
  query that did not flush first missed up to `B` of them. Insertions are applied on the spot
  now, so there is nothing pending and nothing to guard.
- **`top_aae_are_count = Capacity()`** in every MG-family `init` (and `Waving`), so the per-sketch
  top-k AAE/ARE are emitted. The plots use the *all-distinct-key* metrics; the top-k numbers ride
  along for free.
- **No `Delete`** — `delete_sketch` throws.
- **Memory budget → `nslots`** is a binary search over a fresh sketch's `SizeInBytes()` (not a
  one-liner: it is fingerprints plus counters, and the fingerprint width
  depends on `key_bits` = `log2(nslots) + fingerprint_length`). `SpaceSaving` inverts a
  per-monitor estimate instead. Memories only need to match to ±5%; the plots are (actual size,
  error) curves.
- **String workloads (CAIDA)** are hashed to `uint64` in the glue and passed with
  `flag_key_is_hash`; int workloads (kosarak, webdocs) go in raw under `hashmode::Default`.
- Flags on `bench_SublimeMG`: `--fingerprint-length` (default 32), `--growth-coefficient` (`r`),
  `--expand-measure error|total` (picks `SublimeMG<true>`/`<false>` at runtime), `--min-tree`,
  `--no-retune` (leaves VALE on its constructed tuning, i.e. `SetVALERetuning(false)`),
  `--tail-latency` (times every insertion; see `bench_template.hpp`) and `--seed` (0 =
  time-based). `bench_MG` takes `--fingerprint-length`, `--tail-latency` and `--seed`. Both
  report their final VALE tuning (`counters_per_chunk`, `stub_length`) as extra parameters, which
  is what the Fig. 17 VALE table is built from, plus `lost_entries` — the entries the cuckoo
  table's kick paths gave up on, which is silent accuracy loss and wants watching.

**`mg_expansion` (Fig. 18)** shows Sublime_MG improving accuracy across expansions and the memory
win of the error-inducing measure. It plots `SublimeMG<error>` and `SublimeMG<total>` (both
expanding from a small budget with the default **32-bit fingerprints**, `r = 4`, a fixed
`--seed`) against a fixed-size `MG` that just ingests to the end of the stream. The
size-function **power is swept** (0.5 solid, 0.75 dashed, 1.0 dotted, as in the CMS expansion
figure) for both measures, each labelled with its power. **The mult is shared between the two
measures at each power** -- `W(N) = (N/mult)^power` -- which is what guarantees `error <= total`: error-inducing insertions
are a subset of all insertions, so the same size function never expands error more than total.
(Per-measure mults break that, and with it shared the two power-0.5 lines come out identical.)
On WebDocs at power 1.0, `<error>` reaches AAE 3.2 at 7.1 MB while `<total>` needs 155 MB for
AAE 0.011 -- ~22x less memory. Panels are AAE and
Memory[B/key] vs number of keys.

**Dense-start expand workloads.** Because the harness snapshots the whole ground-truth map at every
checkpoint, the measurement period must stay coarse (a fine period on WebDocs OOMs), so the first
checkpoint lands well after the size function has already expanded -- making the sketches look like
they start at different sizes when they all begin at the same budget. `workload_gen`'s opt-in
`--dense-start` adds early checkpoints at doubling stream positions (2048, 4096, 8192, ... so a
power-of-two byte budget like `2^15` lands exactly on one, where the fixed baselines sit at 1
byte/key) before the regular period, so the first plotted point precedes any expansion and the
common starting memory is visible. `mg_expansion` uses dedicated `caida_mg_expand` /
`webdocs_mg_expand` workloads built with it; the paper's plain `caida_expand` / `webdocs_expand`
(used by the CMS/CS `expansion` figure) are left untouched. `plot_mg_expansion` starts the x-axis
at that 1-byte/key checkpoint.

**Fig. 17 plots two error rows**, the AAE over every distinct key and the AAE over the heaviest
`Capacity()` of them, because they answer different questions and only the second one moves with
capacity the way Misra-Gries' bound says it should. Over all distinct keys the mean is dominated
by the tail no summary of that size can hold -- CAIDA has 1.03M distinct keys against a capacity
of 14-19k at 128 KB -- so `Sublime_MG`'s 1.43x capacity over `MG` shows up as an 8.6% better AAE
and a **34% better top-k AAE**. The gap on the all-keys row widens with budget as capacity closes
on the key count (53% at 4 MB), which is the same effect arriving late.

**`mg_tail_latency` (Fig. 19)** is the *worst* insertion rather than the average one: one row,
the same three datasets and budgets as Fig. 17's insert row, on a log axis. It is a **separate
benchmark because it needs separate runs**: `--tail-latency` times every single insertion, which
costs about as much as a cuckoo-table insertion itself, so the average latency such a run reports
is meaningless and must never be read as Fig. 17's. The tail is unharmed -- microseconds of
eviction sweep against tens of nanoseconds of two clock reads -- which is what makes the split
work. All five MG-family benches take the flag; `bench_template.hpp` keeps the histogram (16
buckets an octave) and emits `max_i`, `p99_i`, `p999_i` and `p9999_i` in nanoseconds.

Both experiments are wired through the pipeline like the paper figures: `mg_accuracy_bench` /
`mg_expansion_bench` in `run_benchmarks.py` (the latter runs only on `*_mg_expand` workloads;
both pin a fixed `--seed` on the sketches that support it), `plot_mg_accuracy` (Fig. 17, plus two
companion `.tex` tables -- a P99 table and a VALE `(c, s)` tuning table like the CMS accuracy
figure's) / `plot_mg_expansion` (Fig. 18) in `plot.py`, and `mg_accuracy` / `mg_expansion` figure
options in `generate_datasets.sh` and `evaluate.sh`. `mg_accuracy` reuses the `real` workloads via
the `*"accuracy"*` substring guard; `mg_expansion` triggers generation of the dense-start
workloads under the `*"expansion"*` guard. Error is reported over all distinct keys; P99 goes to
the table. Fig. 17 shares the insert/query rows' y-axes and matches the Sublime_CMS accuracy
figure's panel dimensions.

**CAIDA stays private.** Kosarak and WebDocs come from `download_datasets.sh`; CAIDA is generated
*locally* from `caida_tmp/*.dat` by pointing `generate_datasets.sh`'s real-datasets path at
`caida_tmp` (its `generate_real`/`generate_expand` already pick up `0.dat … 10.dat`). Never stage,
commit, or publish those `.dat` files or reference them in a tracked script.

Note the size function's *units* still differ across the framework: `SublimeCMS`/`SublimeCS` pass a
column count, Sublime_MG a monitored-key `Capacity()`. The shared `--size-function-power` /
`--size-function-mult` lambda therefore means different things to different sketches.

### State of play

Everything above is implemented and tested: 34 `sublime_mg` cases, 13 `vale_counters`, 10 `mg`,
6 `cuckoo_table`, all green under `ctest`, under an assert-enabled build, and under ASan.

**How the rewritten min tree was checked**, because it is the part where a wrong answer is
silent. `VALECountersTest::CheckMinTree` re-derives, by scanning, what *every* internal node ought
to name, not just the root -- which is what catches a climb that stopped a level too early, the
failure mode every one of the shortcuts above could have introduced. `MinTreeMonteCarlo` runs it
after every operation of a random mix of writes, increments, decrements, lazy decrements and group
flushes, at counter counts of 1, 2, 7, 64, 777 and 1000 (mostly not powers of two, which is the
case the levels' short last nodes have to get right). `GroupFlushes` pins down that a lazy
decrement moves nothing and a flush moves no *value* while making the stored counters smaller, and
that one unit is always held back. Each shortcut was added with that harness already in place and
re-run under ASan before it was measured.

**Two things to know when touching the MG suites**, both consequences of the cuckoo table:

- A reference model has to be keyed by an entry's **identity**, never by the bucket it sits in:
  a kick moves an entry between its two candidate buckets, so `it.bucket()` says nothing about
  which key it belongs to. `EntryIdentity` is that identity — the fingerprint plus the smaller
  of the two candidate buckets — and the tests recover it for a *stored* entry as
  `EntryIdentity(it.hash(), flag_key_is_hash)`.
- An insertion into a full table can come back `err_no_space`, and an oracle stops being exact
  the moment the table drops something. So the stream-driven cases go through `InsertOK`, which
  allows that status, and `MGTest::MonteCarlo` follows its oracle in lockstep only up to the
  first loss. **Run the suites with the Release flags too** (`-Ofast -march=native -DNDEBUG`):
  `-Ofast` changes VALE's tuning arithmetic enough to move where a kick path first fails, and
  one case passed the assert-enabled build and failed `ctest` for exactly that reason.

Building the min tree turned up a **latent bug in `VALECounters`**, fixed here and worth knowing
about: `shift_extensions_right_from_pos`, closing the gap a removed extension leaves in a
chunk's pool, did not mask the bits it carried in from the next word, so it corrupted the
extension *below* the one being removed whenever `pos % 64 + shamt` passed 64. That orphans an
extension in the pool, and the chunk's next spill into a tails array then walks the overflow
bitmap past its end — a heap overflow. It needs a pool spanning three 64-bit words
(`counters_per_chunk * (stub_size + 1) < 383`), which no tuning in the committed results
reaches, so nothing measured so far is affected. `PoolShiftKeepsWhatIsBelowIt` covers it.
`ExpectedQuery` re-derives an answer by walking the table itself, and `MGTest`'s `ReferenceMG`
is an independent exact Misra-Gries keyed by `EntryIdentity`, so the tests do not trust the
code they check.

The suites were built by injecting deliberate mutations and requiring each to fail something —
worth continuing, because two real bugs survived the first round of tests and were only caught
that way. **Restore the file
through a shell trap when doing this.** A mutation left live by a crashed run — `retarget()`
deleted from `Expand`, so the threshold never advanced — made `grow_to_fit` double the table
until the machine ran out of memory. That is what the `Capacity() < SizeMeasure()` cap now
prevents, but the harness should not depend on it: run the mutant under `ulimit -v` too.

The benchmark glue and both experiments are implemented (see "Benchmarking Sublime_MG"): `MG`
(10 `mg` cases) and `SpaceSaving` (3 `space_saving` cases) are green under `ctest` and
ASan-clean, built the same mutation-injection way. Both figures have been rerun on the real
datasets under the min tree's rewrite (results `2026-09-28.23:48:49`, figures of the same name),
and once before that under the cuckoo-table rewrite (results `2026-09-25.12:19:25`). What it changed, against the same
32-bit-fingerprint runs of the old design: Sublime_MG is slightly *better* everywhere (the void
bit's removal buys ~3% more capacity per byte) and 20-25% faster to insert, while `MG` improves
sharply (kosarak at 256 KB: AAE 22.1 -> 4.1) and inserts 2.5-3x faster, because it lost the
heap and gained the admit-into-a-freed-slot rule that `MGHeap` never had. So the accuracy gap
between the two narrowed, and what is left of it is VALE's counters alone.

## Compile-time tuning knobs

Four headers are `configure_file` templates (`*.hpp.in` → `build/include/*.hpp`); their VALE
parameters are baked in at compile time so the compiler can specialize the bit-manipulation:

- `FIXED_TUNING_C` (default 64) — counters per chunk, substituted as `COUNTER_PER_CACHE_LINE`.
- `FIXED_TUNING_S` (default 6) — stub length, substituted as `STUB_SIZE`.
- `LOG_REC_INC_PROB` (default 3) — Morris increment probability `2^-p`, `SublimeCMSNoTuningMorris` only.

These affect **only** the `*NoTuning*` variants; `SublimeCMS.hpp` / `SublimeCS.hpp` auto-tune at
runtime and ignore them. `run_benchmarks.py::rebuild_execute_benchmark` re-runs `cmake` + `make`
per data point to sweep them — expect benchmark runs to rebuild repeatedly. Pass its `target`
argument (as `mg_accuracy_bench` does) to rebuild one bench instead of the whole tree, which
turns a per-point rebuild from minutes into seconds. One consequence of sharing the knobs: a run
that sweeps them leaves *every* `*NoTuning*` binary in `build/` compiled for whichever pair was
configured last, so never read one of those binaries' numbers without rebuilding it first.

**`SublimeMGNoTuning.hpp.in` is not a clone.** Sublime_MG's tuning does not live in the sketch,
it lives in `VALECounters`, so there was nothing to duplicate: the generated header `#define`s
`VALE_FIXED_COUNTERS_PER_CHUNK` / `VALE_FIXED_STUB_SIZE` and then includes `SublimeMG.hpp`, and
`VALECounters` declares `counters_per_chunk_`, `stub_size_` and `stub_mask_` as `static constexpr`
when it sees them. Every mask and shift folds, `tune_params` and both retune paths compile out
(`TunesAtRuntime()` says which build you have, and the benches report it as `fixed_tuning`), and
there is one copy of the counter array to maintain rather than two. Two things to know:

- **It only works if that header comes first**, before anything pulls in `VALECounters.hpp`.
  `bench_SublimeMG.cpp` includes it above its own standard headers under
  `#ifdef SUBLIME_MG_FIXED_TUNING`, and `VALECounters.hpp` defines
  `SUBLIME_VALECOUNTERS_INCLUDED` so that getting the order wrong is an `#error` rather than a
  silently runtime-tuned build.
- **`if constexpr` is not enough** for the writes. `VALECounters` is not a template, so a
  discarded `if constexpr` branch still has to compile — and assigning to a `static constexpr`
  member does not. The assignments go through `adopt_tuning`, which is `#ifdef`-ed; the paths
  that only *read* the tuning use `if constexpr`. `Retune(offset)` keeps working in the fixed
  build, because the offset it applies is Misra-Gries' lazy decrement being merged out, which is
  the algorithm and not the tuning.

One bench target serves both builds: `bench_SublimeMGNoTuning` compiles
`sketches_benchmark/bench_SublimeMG.cpp` with `-DSUBLIME_MG_FIXED_TUNING`.

## Architecture

### `include/` — the sketches

| File | Role |
|---|---|
| `SublimeCMS.hpp` | Sublime over Count-Min, with runtime VALE auto-tuning. Reference implementation. |
| `SublimeCS.hpp` | Sublime over Count Sketch. `template <bool l2_size_function>`; when `true`, an AVX-512 AMS sketch estimates the stream's ℓ2 norm (every `ams_sketch_query_period` ops) and drives expansion instead of the key count. |
| `SublimeCMSNoTuning.hpp.in`, `SublimeCMSNoTuningMorris.hpp.in`, `SublimeCSNoTuning.hpp.in` | Fixed-tuning clones of the above for performance measurements. **They are near-duplicates, not includes** — an algorithmic fix in `SublimeCMS.hpp` must usually be mirrored into all of them by hand. |
| `SublimeMGNoTuning.hpp.in` | The same idea for Sublime_MG, done the other way: four lines that `#define` the tuning and include `SublimeMG.hpp`, because the tuning lives in `VALECounters` rather than in the sketch. Nothing is duplicated. See "Compile-time tuning knobs". |
| `CMS.hpp`, `CS.hpp` | Plain Count-Min / Count Sketch baselines. |
| `MG.hpp` | Textbook Misra-Gries baseline: a fixed-size `CuckooTable` (no expansion) with a plain 32-bit counter array that a `CuckooTable::SlotMirror` sidecar keeps aligned as kicks relocate entries. Case 3 is the same sweep `SublimeMG` does — decrement every entry, evict what reaches zero, admit into a slot it freed — with no heap and no bucket list. At a fixed size it is `SublimeMG` minus VALE, and the two agree key for key. |
| `SpaceSaving.hpp` | Space-Saving baseline over the Stream-Summary structure (sorted doubly-linked bucket list + per-bucket monitor lists), pointer-heavy by design. Its **index is a `CuckooTable`**, not a hash map: the same fingerprints the MG family uses, with a monitor pointer beside each slot kept aligned by a `SlotMirror`. No key is stored anywhere — a monitor holds the *slot* that names it, which kicks move — so eviction removes a slot rather than a key. Two consequences it now shares with `MG`: keys whose fingerprint and bucket pair agree share a monitor, and a kick path that gives up drops one. **Its index runs at `index_load_factor = 0.5`**, far below what a cuckoo filter can reach, and that is not a knob to tighten: Space-Saving deletes and re-inserts on *every* miss, so at a full table's load factor the kick paths fail constantly and each failure drops a monitor with a large count on it — 99% of the counted mass, measured. Slack costs it almost nothing next to a 40-byte monitor and a 32-byte bucket, and buys back zero losses. `CountDroppedInsertions` and `CountLostMass` account for every occurrence the monitors do not, which is how the suite keeps "counts sum to N" an equality. |
| `VALECounters.hpp` | A flat VALE counter array (chunk = cache line: overflows bitmap + stubs + extension pool, spilling to a heap tails array), extracted from `SublimeCMS`'s sketch layout. **The range-moving routines are gone** — `ShiftLeft/RightAndClear`, the `repair_range` behind them, and `MinInRange`: they were the quotient filter's, and a cuckoo table moves one counter at a time, so porting them to the rewritten tree would have been real work for capability nothing uses. `git log` has them. Optionally maintains a **min segment tree** over the counters (`VALECounters(n, with_min_tree)`) as a separate packed bit array of within-subtree offsets, 2.29 bits a counter, with the root in a member — so `MinValue()` is the smallest non-empty counter's value and `MinSlot()` a position holding it, each one read; a *stored* zero means *empty*, not a value of zero. With the tree it also owns Misra-Gries' lazy decrement: `IncrementLazy` lowers every count without touching memory, `Get`/`Set` speak counts-plus-`L`, and `MaybeFlushGroup`/`FlushAllGroups` subtract `L` from the counters a group of 32 at a time. See "Insertion: the min segment tree" above, which is where all of that is explained. `MaybeRetune` mirrors `SublimeCMS`'s tails-fraction trigger and needs no tree rebuild, and `RetuneIfNarrower` is the other direction, for counters that have *shrunk* — which spills no chunk and so fires no tails trigger; `ShrinkRetuneInterval()` says how often asking is worth the pass it costs. `DecrementIsZero` decrements and reports emptiness from the overflow bit and the stub alone, without decoding an extension, which is what makes a Misra-Gries decrement sweep affordable. A counter tops out at `2^(32+stub_size)` (tails are `uint32_t`). |
| `SublimeMG.hpp` | Sublime_MG: a `CuckooTable` with counters enabled, so counter `i` is the count of the fingerprint in slot `i`. `Insert` applies all three Misra-Gries cases and `Query` is the matching entry's counter — one entry per key, since every fingerprint is of the same length. `template <bool expand_on_error_inducing_insertions, bool use_min_tree, typename Table>`: with the tree off (the default), case 3 is one sweep that decrements every entry and evicts what it empties; with it on, a lazy decrement `L` and a min tree over the counters make an eviction `O(log w)`, and `drain_emptied` clears two emptied entries on the way out of every insertion. `SetVALERetuning(false)` pins VALE to the tuning it was built with, for measuring what the tuning is worth. Expansion is driven by the size function `expansion_f` handed to the constructor, as in the other Sublime sketches; `template <bool expand_on_error_inducing_insertions = true>` picks whether that size function is read against the insertions that matched nothing or against every insertion — the two Sublime_MG "versions". See the Sublime_MG section above. |
| `CuckooTable.hpp` | The monitored-key set of `SublimeMG` and `MG`. A cuckoo filter with partial-key hashing: `bucket_count` (a power of two) x `depth` slots, slot `b * depth + j`, and `i2 = i1 ^ mix(fingerprint)`. Fingerprint 0 marks an empty slot, so a hash whose fingerprint would be zero uses 1. **Each slot carries a flag bit** saying whether its entry sits in its primary bucket or its alternate, which is the one bit partial-key hashing is short of: with it `hash_of` recovers the whole `key_bits` hash, and a resize is a plain re-hash. **Stretching is depth**: an expansion inside a period gives every bucket `base_depth / r` more slots, and reaching `2 * base_depth` doubles the buckets, halves the depth back and sheds a fingerprint bit. An insertion that finds both buckets full **kicks**, carrying the entry it is displacing *in hand* and swapping it with each slot it passes through — which moves one entry and one counter per kick, and makes a kick path that loops back on itself harmless. A kick path that runs out of patience **drops** what it is carrying, or turns the arrival away with `err_no_space`; `CountLostEntries` reports the first and the benches report both. The constructor takes the largest power-of-two bucket count that *fits* in the `nslots` asked
for and spends what is left on the depth, so a table never overshoots its budget; rounding the
bucket count up instead would make one up to twice the size, which a caller sizing a summary to
a byte budget can only answer by asking for half of it (and did, costing `MG` a third of its
capacity before this was fixed). `BucketOfSlot` is `slot / depth`. `EntryIdentity` is the fingerprint plus the *smaller* of the two candidate buckets, which is exactly the class `FindMatch` can distinguish. |
| `TableHashing.hpp` | The hash functions `CuckooTable` splits its keys with — `MurmurHash64A` and Thomas Wang's integer hash — plus `fpt::bitmask`. Taken verbatim from Memento filter, by way of the quotient filter that used to live here, so that anything built on this hashes the way that did. |
| `util.hpp`, `MurmurHash.hpp` | `BITMASK`/`MAX_VALUE`, `bit_rank`/`bit_select`, `fast_reduce`, extension-length lookup table. |

Public API on every Sublime variant: `Insert`, `Delete`, `Query`, `Size`, `FlushPrefetchQueue`.
Insert/Delete/Query enqueue prefetches, so **queries only see prior updates after
`FlushPrefetchQueue()`** — see `examples/example.cpp`. The constructor takes
`expansion_f`, the *inverse* of the paper's size function `W`: given the current stream measure
it returns the key count that triggers the next expansion/contraction.

Internal vocabulary (matches the paper; comments in `SublimeCMS.hpp:145-265` are the best
reference):

- **chunk** = one cache line (`cache_line_size = 512` bits, a compile-time constant in the class).
- **stub** = the short fixed-width counter prefix; **extension pool** = 2-bit fragments in the
  same chunk holding overflow bits, located via rank/select over a bitmap; **tails array** = the
  heap fallback when a chunk's pool is exhausted. Too many tails arrays
  (`retune_tail_frac`) triggers a **retune** of `(counters_per_chunk, stub_size)` — that pair is
  "VALE".
- **expansion/contraction**: `sketches` is a `std::vector<Sketch *>` of every generation of the
  structure; expansion pushes a new, larger `Sketch` and queries combine generations, so
  contraction can pop back. `Size(include_all_sketches)` reports one generation or all.
- **hash splicing** (`using_tof_hashing`, on by default) picks a chunk with one hash and the
  offsets within it with spliced bits, so a row's update touches a single cache line.

### `bench/` — the evaluation harness

`bench_template.hpp` is the whole harness. Each `sketches_benchmark/bench_<Sketch>.cpp` is thin
glue: define `init_sketch` / `insert_sketch` / `delete_sketch` / `query_sketch` /
`size_of_sketch` (+ optional `get_extra_parameters`) and hand them to `experiment()`,
`experiment_string()`, or `experiment_join_string()` via the `pass_fun` macro. Adding a baseline
= new `bench_X.cpp` + an entry in `bench/CMakeLists.txt`'s `Targets` list and its
`compile_bench` link branch.

`experiment()` replays a binary workload file **twice**: once to build exact ground-truth
frequency checkpoints in a hash map, once to drive the sketch, emitting one JSON object per
`Flush` opcode with `aae`, `are`, over/underestimation, `size`, and `time_<c>` timers. Workloads
are opcode streams (`Insert`/`Delete`/`Timer`/`Flush`/`SwitchTable`) produced by
`workload_gen` (`bench/workload_gen.cpp`, `WorkloadIO` in `bench_utils.hpp`).

### `bench/scripts/` — reproduction pipeline

`evaluate.sh` at the root chains them and writes to a `paper_results/` directory **next to the
repo**, not inside it:

1. `download_datasets.sh` → `paper_results/real_datasets/` (kosarak, webdocs, a 10% CAIDA slice,
   TPC-H `lineitem`/`orders` from Google Drive).
2. `generate_datasets.sh <build> <real_datasets>` → `paper_results/workloads/` via `workload_gen`.
3. `run_benchmarks.py <build> <workloads> -b <names>` → `paper_results/results/<timestamp>/<bench>/<Sketch>_<bytes>_<workload>.json`.
4. `plot.py -f <names>` → `paper_results/figures/<timestamp>/*_(Fig_NN).pdf` (needs LaTeX; `text.usetex=True`).

The benchmark names are the same set at every stage — `accuracy`, `skew`, `vale_tuning`,
`expansion`, `contraction`, `accuracy_unbiased`, `l2_size_function`, `join_size` — keyed by the
`*_bench` function names in `run_benchmarks.py::RUNNERS` and the `plot_*` functions in
`plot.py::PLOTTERS` (where `skew` and `vale_tuning` share one plotter).
A new experiment needs a matching entry in `generate_datasets.sh`, `run_benchmarks.py`, and
`plot.py`. To iterate on one figure without the ~1h full run:

```bash
bash evaluate.sh -f expansion
python3 bench/scripts/plot.py -f expansion -t <timestamp>   # re-plot existing results
```

Memory budgets, VALE parameter pairs, and per-workload sketch sizes are hard-coded tables inside
each `*_bench()` function in `run_benchmarks.py`; plot colors/labels live in
`SKETCHES_STYLE_KWARGS` in `plot.py`.
