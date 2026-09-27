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
counter and the min tree's repair with it. A cuckoo filter relocates one entry per kick and
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
| `SublimeMG` (min tree) | 100.0 | 19016 | 2432 | 1096 ms |

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

- **The tree lives in the counter array**, which doubles: `2n` counters for `n` slots, leaves in
  `[n, 2n)`, internal nodes in `[1, n)`, `parent(i) = i / 2`, index 0 unused. That is the
  bottom-up layout, whose root aggregates every leaf for *any* `n` — which matters, since a slot
  capacity of `buckets * depth` is a power of two only when the depth is. `VALECounters`' public API stays
  slot-indexed and adds `n` internally, so the table needed no change.
- **A zero counter means "no entry here", not a count of zero**, so the combine is `min` over
  the *non-zero* children. A monitored key's counter is always at least `L + 1 >= 1`, so zero is
  unambiguous — and it leaves an empty slot costing a zero stub and nothing else, where a
  `+inf` sentinel would give every one of them an extension or a tails entry.
- **The tree is difference-encoded**: a leaf holds its key's count outright, and an internal
  node holds `excess + 1`, where the excess is how much larger its subtree's minimum is than its
  parent's. So the root holds the global minimum and the sum along a root-to-leaf path is that
  leaf's count. Two consequences. At least one child of every node has an excess of zero — else
  both could be decremented and the parent incremented — so **an increment is a climb of `== 1`
  tests and `Increment`/`Decrement` calls**, with nothing decoded: bump the leaf, read the
  sibling leaf once to learn whether this leaf was the pair's whole minimum, and if it was,
  climb, renormalising by taking one off the sibling and giving it to the parent, stopping the
  moment a sibling reads 1. And the internal nodes now hold *small* numbers, which is what VALE
  is good at: the doubled array costs about 23% more than the leaves alone, not 100%.
- **The `+ 1` is what keeps empty slots out of the minimum.** A plain difference encoding would
  need an excess of infinity for an entirely empty subtree; shifting by one frees the value 0 to
  mean "nothing here" while leaving every test as cheap.
- **A general write** — an admission or an eviction, which can move a minimum anywhere — is told
  what the leaf held before, so it derives the minima the differences are relative to on the way
  *up* (a node's parent's minimum is its own less its difference) and stops at the first node
  that does not move. Only a node that was empty has nothing to derive from, and that falls back
  to a walk from the root.
- **A leaf count is rounded up to even**, so that no node has one leaf child and one internal
  one and a climb never has to ask which kind a sibling is. An odd count gets a spare, empty leaf.
- **`VALECounters::repair_range` now has no caller.** It exists for a table that moves a *range*
  of counters at once, which the RSQF's shifts did and the cuckoo table's kicks do not — a kick
  is one `Set` per relocated entry, which the ordinary update handles. It and the two
  `Shift*AndClear` routines are kept, and still tested, because they are the general
  array's business rather than any one table's; see the `VALECounters.hpp` row. Their cost is
  what the cuckoo table bought us: the tree over the RSQF was the slowest of the four
  configurations by 4.6x, because every shifted slot dragged a repair with it.
- **The eviction candidate is tracked, not searched for.** `MinSlot()` is a leaf holding the
  root's value, maintained on every write: a write that attains the root becomes the candidate
  (which is how an admission becomes the next eviction), and a write that lifts the candidate
  off the minimum descends from *the node the climb stopped at* — whose other child holds the
  minimum — rather than from the root. A slot shift only moves the candidate's index.
- **`CuckooTable::BucketOfSlot` is `slot / depth`**, which is what turns the tree's answer back
  into something the table can delete: the tree hands back a slot and `DeleteSlot` wants the
  bucket with it. Over the RSQF this was an `O(cluster)` walk back to the start of the slot's
  cluster, pairing runs with occupied buckets forward.
- **Case 3 pays for a decrement only when nothing is already at zero** (`MinValue() > L`), and
  then evicts **everything** the decrement emptied, not just the one entry the arrival needs.
  Both halves matter. The first keeps `stored >= L` an invariant, which is what makes the merge
  below safe — it is unreachable while the batch is unbounded, but `eviction_batch` is a
  constant and a finite one would need it. The second is a performance cliff, not a nicety:
  capping the batch leaves the table hovering at its 0.95 load factor, where every insertion and
  deletion walks a long cluster, and on kosarak at 16 KB the same ~2.2M evictions cost **24.5 s
  in batches of 256 against 3.3 s unbounded**, for identical answers.
- **The lazy decrement is merged out** once it passes half of what a stub can hold, as VALE's
  rebuild offset — free, since the rebuild reads and rewrites every counter anyway. What comes
  off is `L - 1`, leaving `L = 1`: an entry whose count has reached zero sits at exactly `L`,
  and taking the whole of `L` off it would store zero, which the tree reads as an empty slot.
- **What it costs.** The counters double, so at a fixed budget the summary monitors fewer keys
  (kosarak at 16 KB: 1716 against 2279) and is correspondingly less accurate (AAE 108.8 against
  100.0). Insertion is still ~2.5x slower than the sweep there (1132 ms against 451 ms), because
  the sweep's bulk eviction leaves the table slack that the tree has to work harder for. What
  the tree buys is the *worst case*: `O(log w)` to find and evict, against `O(w)`.

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
the min tree; `run_benchmarks.py`'s `mg_accuracy_bench` names the configurations and
`execute_benchmark`'s `label` argument is what keeps two runs of one binary in separate files. **`MG` and `SublimeMG` are the same algorithm over the same monitored set**
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

Everything above is implemented and tested: 34 `sublime_mg` cases, 17 `vale_counters`, 10 `mg`,
6 `cuckoo_table`, all green under `ctest`, under an assert-enabled build, and under ASan.

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
datasets under the rewrite (results `2026-09-25.12:19:25`). What it changed, against the same
32-bit-fingerprint runs of the old design: Sublime_MG is slightly *better* everywhere (the void
bit's removal buys ~3% more capacity per byte) and 20-25% faster to insert, while `MG` improves
sharply (kosarak at 256 KB: AAE 22.1 -> 4.1) and inserts 2.5-3x faster, because it lost the
heap and gained the admit-into-a-freed-slot rule that `MGHeap` never had. So the accuracy gap
between the two narrowed, and what is left of it is VALE's counters alone.

## Compile-time tuning knobs

Three headers are `configure_file` templates (`*.hpp.in` → `build/include/*.hpp`); their VALE
parameters are baked in at compile time so the compiler can specialize the bit-manipulation:

- `FIXED_TUNING_C` (default 64) — counters per chunk, substituted as `COUNTER_PER_CACHE_LINE`.
- `FIXED_TUNING_S` (default 6) — stub length, substituted as `STUB_SIZE`.
- `LOG_REC_INC_PROB` (default 3) — Morris increment probability `2^-p`, `SublimeCMSNoTuningMorris` only.

These affect **only** the `*NoTuning*` variants; `SublimeCMS.hpp` / `SublimeCS.hpp` auto-tune at
runtime and ignore them. `run_benchmarks.py::rebuild_execute_benchmark` re-runs `cmake` + `make`
per data point to sweep them — expect benchmark runs to rebuild the tree repeatedly.

## Architecture

### `include/` — the sketches

| File | Role |
|---|---|
| `SublimeCMS.hpp` | Sublime over Count-Min, with runtime VALE auto-tuning. Reference implementation. |
| `SublimeCS.hpp` | Sublime over Count Sketch. `template <bool l2_size_function>`; when `true`, an AVX-512 AMS sketch estimates the stream's ℓ2 norm (every `ams_sketch_query_period` ops) and drives expansion instead of the key count. |
| `SublimeCMSNoTuning.hpp.in`, `SublimeCMSNoTuningMorris.hpp.in`, `SublimeCSNoTuning.hpp.in` | Fixed-tuning clones of the above for performance measurements. **They are near-duplicates, not includes** — an algorithmic fix in `SublimeCMS.hpp` must usually be mirrored into all of them by hand. |
| `CMS.hpp`, `CS.hpp` | Plain Count-Min / Count Sketch baselines. |
| `MG.hpp` | Textbook Misra-Gries baseline: a fixed-size `CuckooTable` (no expansion) with a plain 32-bit counter array that a `CuckooTable::SlotMirror` sidecar keeps aligned as kicks relocate entries. Case 3 is the same sweep `SublimeMG` does — decrement every entry, evict what reaches zero, admit into a slot it freed — with no heap and no bucket list. At a fixed size it is `SublimeMG` minus VALE, and the two agree key for key. |
| `SpaceSaving.hpp` | Space-Saving baseline over the Stream-Summary structure (sorted doubly-linked bucket list + per-bucket monitor lists + hash map), storing full 64-bit keys. Pointer-heavy by design. |
| `VALECounters.hpp` | A flat VALE counter array (chunk = cache line: overflows bitmap + stubs + extension pool, spilling to a heap tails array), extracted from `SublimeCMS`'s sketch layout. `ShiftLeft/RightAndClear` move a range by one slot touching only stubs, the bitmap, and any tails array — the pool is ordered by counter position, so a shift by one leaves it bit-identical and only the counters *crossing a chunk boundary* need their extensions moved. **Those two, and the tree's `repair_range` behind them, have no caller left**: they were the quotient filter's, and a cuckoo table moves one counter at a time. They are kept and still tested, as the array's own generality. Optionally carries a **min segment tree** over the counters (`VALECounters(n, with_min_tree)`), which doubles the array and lays a bottom-up tree over it — leaves in `[n, 2n)`, `parent(i) = i / 2` — so `MinValue()` is the smallest non-zero counter and `MinSlot()` a leaf holding it, both maintained through writes and slot shifts; a zero counter means *empty*, not a count of zero. `MaybeRetune` mirrors `SublimeCMS`'s tails-fraction trigger, `Retune(offset)` subtracts a uniform offset as it rebuilds (Misra-Gries' lazy decrement, applied for free), and `RetuneIfNarrower` is the other direction, for counters that have *shrunk* — which spills no chunk and so fires no tails trigger; `ShrinkRetuneInterval()` says how often asking is worth the pass it costs. `DecrementIsZero` decrements and reports emptiness from the overflow bit and the stub alone, without decoding an extension, which is what makes a Misra-Gries decrement sweep affordable. A counter tops out at `2^(32+stub_size)` (tails are `uint32_t`). |
| `SublimeMG.hpp` | Sublime_MG: a `CuckooTable` with counters enabled, so counter `i` is the count of the fingerprint in slot `i`. `Insert` applies all three Misra-Gries cases and `Query` is the matching entry's counter — one entry per key, since every fingerprint is of the same length. `template <bool expand_on_error_inducing_insertions, bool use_min_tree, typename Table>`: with the tree off (the default), case 3 is one sweep that decrements every entry and evicts what it empties; with it on, a lazy decrement `L` and a min tree over the counters make an eviction `O(log w)`. `SetVALERetuning(false)` pins VALE to the tuning it was built with, for measuring what the tuning is worth. Expansion is driven by the size function `expansion_f` handed to the constructor, as in the other Sublime sketches; `template <bool expand_on_error_inducing_insertions = true>` picks whether that size function is read against the insertions that matched nothing or against every insertion — the two Sublime_MG "versions". See the Sublime_MG section above. |
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
