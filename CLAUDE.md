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
ctest -R sublime_cms      # one suite: cms | sublime_cms | cs | sublime_cs |
                          #            fingerprint_table | vale_counters | sublime_mg | all
./tests/SublimeCMSTests --test-case="monte carlo"        # one doctest case
```

`ctest` builds Release, which defines `NDEBUG` — so every `assert` in the headers is compiled
out and the suites never check the internal invariants (`bits_per_slot_ <= 56` in
`FingerprintTable::allocate`, `chain_sum > lazy_decrement_` in `SublimeMG`, ...). Worth a
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
`FingerprintTableTest`, `VALECountersTest`, and `SublimeMGTest` do the same. Add
new white-box checks as static methods on that friend class and wire them into a `TEST_CASE`.

## Sublime_MG (journal extension, in progress)

The `journal_extension` branch is building Sublime_MG — Sublime applied to Misra-Gries — in
steps. What exists: `FingerprintTable` (the monitored-key set), `VALECounters` (the counts),
and the wiring between them in `SublimeMG`. The table mirrors every slot it shifts into the
counter array via `AttachCounters`, which is also what makes the counter prefetch in
`FindLongestMatch` possible.

The table *owns* its counters (`EnableCounters`, not an attach-a-pointer arrangement), because
the two must be resized in lockstep and only the table knows when that happens. `Expand` and
`Contract` carry every count across with its fingerprint; when a period-ending expansion
duplicates a void entry into two buckets, **both copies keep the whole count** — neither has a
fingerprint left to tell it from the other, so a query may land on either and only the full
count keeps the answer an over-estimate. Contraction does not merge those copies back.

### Chains — the one idea the rest follows from

A key's count is **not one counter but a sum**: every fingerprint of its run that matches it,
of whatever length, holds a share of it. Runs are held in ascending slot value and the void bit
outweighs the fingerprint, so a run is in ascending order of *length*, and an earlier entry of
a run is a prefix of a later one exactly when `fingerprints_match` says the two match. The
entries matching a key therefore form a **chain**, and the count of the key family ending at
entry `e` is its **chain sum** — `e`'s counter plus the counters of the earlier matching
entries of its run. `FingerprintTable::ForEachEntryWithChainSum` is the one-sweep-per-run pass
that produces them.

- `Query(k)` = chain sum of `k`'s longest match, less `lazy_decrement_` **once**. The decrement
  is owed by the *key*, not by each entry its count is spread over — subtracting it per match
  would underflow. `Count` no longer exists; `Query` is the only estimator.
- **Admitting** stores `L + count` when nothing matched the key (its entry is the whole chain,
  so it carries the decrement), and just `count` when something did (the chain already carries
  it further down).
- **Merging** `lazy_decrement_` subtracts it from the *first entry of each chain only* —
  the entries whose `chain_sum == count`. Subtracting it from every counter, as a uniform
  `Retune(offset)` would, takes it off once per entry instead of once per chain.
- **Deleting** an entry takes its counter out of the chain sums of the longer entries that
  matched it, which would silently cut their keys' counts.
  `FingerprintTable::DeleteEntriesPreservingChainSums` repairs that by handing what was
  removed to the entries left at the *bottom* of each chain, and to those only — everything
  above reaches the removed counters through them. It is purely additive, so no counter can go
  negative. Its victims must be downward-closed per run, which anything chosen by chain sum is.
- Note `GetNumFingerprintBits()` does *not* shrink on expansion — it is the length of a
  *freshly inserted* fingerprint, and `key_bits` grows alongside the bucket index; it is the
  stored entries that lose a bit (`rebuild_op::sacrifice`). That is how fingerprints of
  different lengths, and so chains, come to exist at all.

### Insertion, the buffer, and the flush

- **Only `Insert`, `Query`, `FlushBuffer`, `Reset`, `Expand`/`Contract` and the read-only
  accessors are public.** `StartMonitoring`/`StopMonitoring`/`MergeLazyDecrement` are private
  bare primitives (the tests are a `friend`): each can leave the summary in a state the
  algorithm would never produce. They flush the buffer first, which is what keeps the flush's
  own assumption true — a buffered key matched nothing when it was buffered, and still matches
  nothing when it is applied.
- **Buffered insertions are not visible until `FlushBuffer()`** (or until the buffer fills),
  the same convention the other Sublime sketches use for their prefetch queues. Tests and
  benchmarks must flush before querying.
- **`FlushBuffer` is one replay plus one sweep, never a loop.** Pass one takes the `B` smallest
  chain sums. The replay works the batch out without touching the table: a key already pending
  under the same `EntryIdentity` counts that one up, a key with a slot free takes it, and
  otherwise `dec` goes up, everything the decrement has caught up with makes way — table
  entries *and* pending keys of this very batch, which are admitted at a count of one and so
  can be decremented back out before the batch ends, exactly as Misra-Gries has it — and the
  occurrence that paid takes one of the slots it freed. It never runs out of room first:
  `free = initial + died - a`, so with all `B` candidates dead and a key still waiting,
  `a <= B - 1` and `free >= 1`. Then `L += dec`, and pass two evicts every entry with
  `chain_sum <= L`, which subsumes the candidates and sweeps up the ties the first pass never
  looked at.
- **The counter array is rebuilt in exactly one place**, `SublimeMG::rebuild()`. VALE wants a
  new tuning when chunks spill into tails arrays; the lazy decrement wants subtracting out once
  it is dead weight. The subtraction is its own pass now (chain roots only, over the entries),
  and `Retune(0, shrank=true)` follows: `shrank` tells VALE the counters really did get smaller,
  which lifts the forced decrease of `counters_per_chunk` that a plain zero-offset retune
  applies to stop a tails-driven retune landing back where it started.

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

- `FingerprintTable::CountSlotsAfterExpansion` / `CountSlotsAfterContraction` predict a resize
  from the shape arithmetic alone, without building the table. `SublimeMG` uses the first to
  retire the threshold when the table has no hash bits left to spend, so a summary that cannot
  grow stops testing rather than failing an expansion per insertion.
- Whether an expansion is a stretch inside the period or the doubling that ends it is
  `FingerprintTable`'s business; the policy only reads what it left behind.
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
| make pending updates visible | `FlushBuffer()` | `FlushPrefetchQueue()` |
| space | `SizeInBytes()` | `Size(bool include_all_sketches)` |
| delete | *(none — Misra-Gries has no deletions)* | `Delete(...)` |

Constructor: `SublimeMG<E>(nslots, key_bits, hash_mode, seed, growth_coefficient = 1,
buffer_capacity = 64, expansion_f = {})`. `Capacity()` is `nslots * max_load_factor` (0.95).
`flags` takes `flag_key_is_hash` when the caller has already hashed the key, so a string
workload has to be hashed to a `uint64_t` first. `hashmode` is `Default` (Murmur),
`Invertible` (hash as wide as the key), or `None` (key used as its own hash — skew in the
input becomes skew in the load). `Insert` returns 0 or a negative status (`err_no_space`,
`err_not_monitored`). Copy and move both work, unlike `MGDummy`, which deletes them. `Reset`
empties the summary but **keeps the size it grew to**, and re-derives the threshold from it.

Two shape constraints, both `assert`-only and therefore **silent in the Release/NDEBUG build**:

- `key_bits - log2(nslots) + 1 <= 56` — a slot has to be readable by one 64-bit word — i.e.
  `key_bits <= 55 + log2(nslots)`. Overshooting it passes `ctest` and trips in the
  assert-enabled build; that is exactly how the one bad test configuration was caught.
- A period-ending expansion needs one more hash bit, so growth stops once `key_bits` reaches
  63. `CountSlotsAfterExpansion() == CountSlots()` is how the summary notices and retires its
  threshold.

### Benchmarking Sublime_MG

**There is no `bench_SublimeMG.cpp` yet** — that is the next step. It is the usual thin glue
plus an entry in `bench/CMakeLists.txt`'s `Targets` list and its `compile_bench` link branch.
Four things that will bite:

- **`query_sketch` must call `FlushBuffer()` first.** The harness calls `query_f` directly at
  every `Flush` opcode and never flushes the sketch itself, so up to `B = 64` insertions would
  be invisible and the measured error would be wrong. This is the one glue mistake that
  produces plausible-looking but incorrect numbers.
- **Set `top_aae_are_count`** to `Capacity()` in `init_sketch`, as `bench_MGDummy.cpp` does.
  A Misra-Gries summary only claims to answer for the heavy keys; left at its default the
  harness averages AAE/ARE over every key in the checkpoint.
- **No `Delete`.** Follow `bench_MGDummy.cpp` and throw from `delete_sketch`.
- **Memory budget → `nslots`** is not the one-liner it is for the counter-array sketches:
  `SizeInBytes()` is fingerprints + counters + buffer, and the fingerprint width itself depends
  on `key_bits` and `nslots`.

`MGDummy` is the baseline, and it differs from Sublime_MG in **two** ways that each move the
size curve. Control for both or the space plots will not be comparable:

1. **What is counted.** `MGDummy` tests its threshold against every insertion; `SublimeMG<>`
   tests against the error-inducing ones. Use `SublimeMG<false>` for a like-for-like run, or
   give each a size function written for its own measure.
2. **Which side of the step the threshold sits on.** `MGDummy` grows one slot at a time and
   sets `expansion_lim = expansion_f(max_slot_count + 1)` — the size *after* the step — so its
   size tracks `W(N)` almost exactly. Sublime_MG grows by a factor of `2^(1/r)` and uses
   `expansion_f(Capacity())` — the size *before* — so it sits *above* `W(N)` by up to that
   factor and never below, which is what keeps it from being under-sized for its error
   guarantee. The two conventions coincide when the step is one slot, which is why `MGDummy`
   can use either; they do not coincide at `r = 1`. **Raising `r` is what closes the gap** —
   that is what the growth coefficient is for, and it is the knob to sweep before concluding
   Sublime_MG is bigger than the baseline. Moving to the other side is one line in
   `SublimeMG::retarget()`.

Note also that the size function's *units* differ across the framework: `SublimeCMS`/
`SublimeCS` pass a column count, `MGDummy` a slot count, Sublime_MG a monitored-key
`Capacity()`. The harness's `--size-function-power`/`--size-function-mult` lambda is shared, so
the same flags mean different things to different sketches.

### State of play

Everything above is implemented and tested: 41 `sublime_mg` cases, 24 `fingerprint_table`, 13
`vale_counters`, all green under `ctest`, under an assert-enabled build, and under valgrind
(no leaks, no errors). `CheckSummary` re-derives every chain sum independently of the
table's own sweep, so the tests do not trust the code they check.

The suites were built by injecting deliberate mutations and requiring each to fail something —
worth continuing, because two real bugs (the eviction repair applied to every survivor rather
than the newly rootless ones, and the candidate pass reading raw counters instead of chain
sums) survived the first round of tests and were only caught that way. **Restore the file
through a shell trap when doing this.** A mutation left live by a crashed run — `retarget()`
deleted from `Expand`, so the threshold never advanced — made `grow_to_fit` double the table
until the machine ran out of memory. That is what the `Capacity() < SizeMeasure()` cap now
prevents, but the harness should not depend on it: run the mutant under `ulimit -v` too.

Next step is the benchmark glue above. Nothing in the algorithm is left open.

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
| `CMS.hpp`, `CS.hpp`, `MGDummy.hpp` | Plain baselines. `MGDummy` models Misra-Gries' memory only. |
| `VALECounters.hpp` | A flat VALE counter array (chunk = cache line: overflows bitmap + stubs + extension pool, spilling to a heap tails array), extracted from `SublimeCMS`'s sketch layout. `ShiftLeft/RightAndClear` move a range by one slot touching only stubs, the bitmap, and any tails array — the pool is ordered by counter position, so a shift by one leaves it bit-identical and only the counters *crossing a chunk boundary* need their extensions moved. `MaybeRetune` mirrors `SublimeCMS`'s tails-fraction trigger. `Retune(offset, shrank)` subtracts a uniform `offset` as it rebuilds; `shrank` says the caller already made the counters smaller itself, which frees the tuning the same way a non-zero offset does. A counter tops out at `2^(32+stub_size)` (tails are `uint32_t`). |
| `SublimeMG.hpp` | Sublime_MG: a `FingerprintTable` with counters enabled, so counter `i` is the count of the fingerprint in slot `i`. `Insert` applies all three Misra-Gries cases; `Query` sums the *chain* of matching fingerprints and takes `lazy_decrement_` off once. Case-3 insertions batch into a `B`-entry buffer; a flush takes one pass for the `B` smallest chain sums, replays the batch against them, and sweeps up everything the decrement emptied. Expansion is driven by the size function `expansion_f` handed to the constructor, as in the other Sublime sketches; `template <bool expand_on_error_inducing_insertions = true>` picks whether that size function is read against the insertions that matched nothing or against every insertion. **Work in progress** — see below. |
| `FingerprintTable.hpp` | Compact hash table for the in-progress Sublime_MG: an RSQF with Memento filter's hashing, Aleph filter's variable-length fingerprints, and Zeno filter's Stretching; mementos removed. One fingerprint per slot. With growth coefficient `r` (ctor arg, default 1 = plain doubling) `Expand` grows by `2^(1/r)`, mapping base bucket `i` to `floor(i * 2^(epoch/r))`; only the `r`-th expansion of a *period* trades a fingerprint bit for a bucket-index bit. `CountSlotsAfterExpansion`/`CountSlotsAfterContraction` predict either without building the table, for callers that decide on a resize before making it. |
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
