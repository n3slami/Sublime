#include <algorithm>
#include <map>
#include <random>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>

#include "VALECounters.hpp"

namespace sublime {

/**
 * White-box tests for `VALECounters`. Declared a `friend`, so these reach into
 * the chunk layout, the extension pool, and the tuning directly.
 */
class VALECountersTest {
public:
    /** @returns The raw contents of a chunk's extension pool. */
    static std::vector<uint64_t> Pool(const VALECounters& t, uint64_t chunk) {
        std::vector<uint64_t> res(t.num_extension_words_);
        uint64_t unpacked[VALECounters::max_extension_words];
        t.read_extensions(t.chunk_words(chunk), unpacked);
        for (uint32_t i = 0; i < t.num_extension_words_; i++)
            res[i] = unpacked[i];
        return res;
    }

    static bool HasTails(const VALECounters& t, uint64_t chunk) {
        return t.has_tails_array(t.chunk_words(chunk));
    }

    /** @returns The overflows bitmap of a chunk, as one bit per counter. */
    static std::vector<bool> Overflows(const VALECounters& t, uint64_t chunk) {
        std::vector<bool> res(t.counters_per_chunk_);
        for (uint32_t i = 0; i < t.counters_per_chunk_; i++)
            res[i] = t.is_overflowing(t.chunk_words(chunk), i);
        return res;
    }

    /**
     * Checks the array against a reference vector, and that the overflows
     * bitmap agrees with the values: a counter is marked overflowing exactly
     * when it does not fit in a stub.
     */
    static void CheckAgainst(const VALECounters& t, const std::vector<uint64_t>& ref) {
        REQUIRE_EQ(t.CountCounters(), ref.size());
        for (uint64_t i = 0; i < ref.size(); i++) {
            REQUIRE_EQ(t.Get(i), ref[i]);
            const uint64_t chunk = i / t.counters_per_chunk_;
            const uint32_t inter = i - chunk * t.counters_per_chunk_;
            REQUIRE_EQ(t.is_overflowing(t.chunk_words(chunk), inter),
                       ref[i] > MAX_VALUE(t.stub_size_));
        }
    }

    static void GetAndSet() {
        VALECounters t(1000);
        REQUIRE_EQ(t.GetCountersPerChunk(), VALECounters::default_counter_per_cache_line);
        REQUIRE_EQ(t.GetStubLength(), VALECounters::default_stub_size);
        REQUIRE_EQ(t.CountChunks(), (1000 + t.GetCountersPerChunk() - 1) / t.GetCountersPerChunk());

        // A fresh array is all zeros.
        std::vector<uint64_t> ref(1000, 0);
        CheckAgainst(t, ref);

        // Values that fit in a stub, values that need an extension, and values
        // long enough to need several fragments.
        const std::vector<uint64_t> values = {0, 1, 31, 32, 33, 100, 1000, 100000,
                                              1ULL << 20, (1ULL << 31) - 1};
        for (uint64_t i = 0; i < 1000; i++) {
            ref[i] = values[i % values.size()];
            t.Set(i, ref[i]);
        }
        CheckAgainst(t, ref);

        // Overwriting a counter, in both directions across the stub boundary,
        // leaves its neighbours alone.
        std::mt19937_64 rng(1);
        for (uint64_t i = 1; i + 1 < 1000; i += 7) {
            ref[i] = rng() % (1ULL << 24);
            t.Set(i, ref[i]);
            REQUIRE_EQ(t.Get(i - 1), ref[i - 1]);
            REQUIRE_EQ(t.Get(i), ref[i]);
            REQUIRE_EQ(t.Get(i + 1), ref[i + 1]);
        }
        CheckAgainst(t, ref);

        // And setting everything back to zero empties the pools again. A
        // chunk that spilled keeps its tails array until a retune reclaims it,
        // so its pool still holds the pointer.
        for (uint64_t i = 0; i < 1000; i++)
            t.Set(i, 0);
        std::fill(ref.begin(), ref.end(), 0);
        CheckAgainst(t, ref);
        for (uint64_t c = 0; c < t.CountChunks(); c++) {
            if (HasTails(t, c))
                continue;
            for (const uint64_t word : Pool(t, c))
                REQUIRE_EQ(word, 0);
        }
    }

    static void IncrementAndDecrement() {
        VALECounters t(200);
        // Counting one counter up past the stub and through several extension
        // fragments, checking every single step.
        const uint64_t target = 5000;
        for (uint64_t i = 1; i <= target; i++) {
            t.Increment(7);
            REQUIRE_EQ(t.Get(7), i);
        }
        // Its neighbours never moved.
        REQUIRE_EQ(t.Get(6), 0);
        REQUIRE_EQ(t.Get(8), 0);

        for (uint64_t i = target; i > 0; i--) {
            REQUIRE_EQ(t.Get(7), i);
            t.Decrement(7);
        }
        REQUIRE_EQ(t.Get(7), 0);
        REQUIRE_FALSE(t.is_overflowing(t.chunk_words(0), 7));

        // Several counters of one chunk growing together, so their extensions
        // interleave in the pool.
        std::vector<uint64_t> ref(200, 0);
        std::mt19937_64 rng(2);
        for (int32_t step = 0; step < 20000; step++) {
            const uint64_t i = rng() % 200;
            if (ref[i] > 0 && (rng() % 4) == 0) {
                t.Decrement(i);
                ref[i]--;
            }
            else {
                t.Increment(i);
                ref[i]++;
            }
        }
        CheckAgainst(t, ref);
    }

    /** A chunk whose extensions outgrow its pool moves them to a tails array. */
    static void SpillsIntoTailsArray() {
        VALECounters t(200);
        const uint32_t cpc = t.GetCountersPerChunk();
        REQUIRE_FALSE(HasTails(t, 0));

        // Big values, so that a handful of them exhausts the pool.
        std::vector<uint64_t> ref(200, 0);
        for (uint32_t i = 0; i < cpc; i++) {
            ref[i] = 1000000ULL + i;
            t.Set(i, ref[i]);
            CheckAgainst(t, ref);
            if (HasTails(t, 0))
                break;
        }
        REQUIRE(HasTails(t, 0));
        REQUIRE_EQ(t.CountChunksWithTails(), 1);
        // The values that were in the pool survived the move.
        CheckAgainst(t, ref);

        // A chunk with a tails array keeps working, for every operation.
        for (uint32_t i = 0; i < cpc; i++) {
            t.Increment(i);
            ref[i]++;
        }
        CheckAgainst(t, ref);
        for (uint32_t i = 0; i < cpc; i++) {
            t.Set(i, i % 3 == 0 ? 0 : 7);
            ref[i] = i % 3 == 0 ? 0 : 7;
        }
        CheckAgainst(t, ref);
        // The tails array outlives its contents; only a retune reclaims it.
        REQUIRE(HasTails(t, 0));
    }

    /*
     * ------------------------------------------------------------------
     * Shifting.
     * ------------------------------------------------------------------
     */

    /*
     * ------------------------------------------------------------------
     * The extension pool's own shifts.
     * ------------------------------------------------------------------
     */

    /** Re-shapes `t` to a tuning of our choosing, for a white-box test. */
    static void Reshape(VALECounters& t, uint64_t counters, uint32_t cpc, uint32_t stub) {
        t.free_tails();
        delete[] t.chunks_;
        t.allocate_with(counters, cpc, stub);
    }

    /**
     * Shifting a chunk's extension pool down from `pos` must leave everything
     * *below* `pos` exactly as it was. It is a bit-level operation on a pool
     * that can span three 64-bit words, and the bits carried in from the next
     * word land `shamt` below the top -- which reaches under `pos` as soon as
     * `pos % 64 + shamt` passes 64. Getting that wrong corrupts a neighbouring
     * extension and leaves the pool holding one more extension than the
     * overflow bitmap admits to, which is a heap overflow later on, when the
     * chunk spills into a tails array and walks the bitmap to place them.
     */
    static void PoolShiftKeepsWhatIsBelowIt() {
        VALECounters t(64);
        // A tuning whose pool needs three words: 512 - 1 - 76 * 5 = 131 bits.
        Reshape(t, 4096, 76, 4);
        REQUIRE_EQ(t.num_extension_words_, 3);
        const uint32_t words = t.num_extension_words_;

        std::mt19937_64 rng(4242);
        for (uint32_t trial = 0; trial < 2000; trial++) {
            uint64_t pool[VALECounters::max_extension_words] = {};
            uint64_t ref[VALECounters::max_extension_words] = {};
            for (uint32_t i = 0; i < words; i++)
                pool[i] = ref[i] = rng();

            const uint32_t pos = rng() % (words * 64 - 2);
            const uint32_t shamt = 2 + 2 * (rng() % 20);        // A whole number of fragments.
            t.shift_extensions_right_from_pos(pool, pos, shamt);

            // The reference: bit `b` of the result is bit `b + shamt` of the
            // original for `b >= pos`, and bit `b` of it below that.
            const auto bit_of = [&](const uint64_t *w, uint32_t b) {
                return b >= words * 64 ? 0ULL : (w[b / 64] >> (b % 64)) & 1ULL;
            };
            for (uint32_t b = 0; b < words * 64; b++) {
                const uint64_t want = b < pos ? bit_of(ref, b) : bit_of(ref, b + shamt);
                REQUIRE_EQ(bit_of(pool, b), want);
            }
        }
    }

    /** The same, for the shift that opens a gap rather than closing one. */
    static void PoolShiftUpKeepsWhatIsBelowIt() {
        VALECounters t(64);
        Reshape(t, 4096, 76, 4);
        const uint32_t words = t.num_extension_words_;

        std::mt19937_64 rng(4243);
        for (uint32_t trial = 0; trial < 2000; trial++) {
            uint64_t pool[VALECounters::max_extension_words] = {};
            uint64_t ref[VALECounters::max_extension_words] = {};
            for (uint32_t i = 0; i < words; i++)
                pool[i] = ref[i] = rng();

            const uint32_t pos = rng() % (words * 64 - 2);
            const uint32_t shamt = 2 + 2 * (rng() % 20);
            t.shift_extensions_left_from_pos(pool, pos, shamt);

            const auto bit_of = [&](const uint64_t *w, uint32_t b) {
                return (w[b / 64] >> (b % 64)) & 1ULL;
            };
            for (uint32_t b = 0; b < words * 64; b++) {
                uint64_t want;
                if (b < pos)
                    want = bit_of(ref, b);
                else if (b < pos + shamt)
                    want = 0;
                else
                    want = bit_of(ref, b - shamt);
                REQUIRE_EQ(bit_of(pool, b), want);
            }
        }
    }

    /*
     * ------------------------------------------------------------------
     * The min segment tree.
     * ------------------------------------------------------------------
     */

    /** @returns The smallest non-zero value of `ref`, or 0 if there is none. */
    static uint64_t SmallestNonZero(const std::vector<uint64_t>& ref) {
        uint64_t res = 0;
        for (const uint64_t v : ref)
            if (v != 0 && (res == 0 || v < res))
                res = v;
        return res;
    }

    /** The tree's root is that minimum, and its candidate is a leaf holding it. */
    static void CheckMinTree(const VALECounters& t, const std::vector<uint64_t>& ref) {
        const uint64_t smallest = SmallestNonZero(ref);
        REQUIRE_EQ(t.MinValue(), smallest);
        if (smallest != 0)
            REQUIRE_EQ(ref[t.MinSlot()], smallest);
        for (uint64_t i = 0; i < ref.size(); i++)
            REQUIRE_EQ(t.Get(i), ref[i]);
        // Every internal node holds its subtree's minimum outright, so each one
        // can be checked against its two children directly -- which catches a
        // climb that stopped a level too early, where `MinValue` alone only
        // catches the ones that reach the root. The root is in a member.
        for (uint64_t x = t.leaf_count_ - 1; x >= 1; x--) {
            const uint64_t left = t.get_raw(2 * x), right = t.get_raw(2 * x + 1);
            const uint64_t want = left == 0 ? right
                                            : (right == 0 ? left : std::min(left, right));
            REQUIRE_EQ(x == 1 ? t.root_min_ : t.get_raw(x), want);
            if (x == 1)
                break;
        }
        for (uint64_t i = 0; i < t.leaf_count_; i += 2) {
            const uint64_t left = i < ref.size() ? ref[i] : 0;
            const uint64_t right = i + 1 < ref.size() ? ref[i + 1] : 0;
            REQUIRE_EQ(t.not_pair_minimum(t.leaf_count_ + i), right != 0 && right <= left);
            REQUIRE_EQ(t.not_pair_minimum(t.leaf_count_ + i + 1), left != 0 && left <= right);
        }
    }

    /**
     * Every operation, against a brute-force minimum. The leaf count is not a
     * power of two in most of these, which is the case the bottom-up layout
     * has to get right.
     */
    static void MinTreeMonteCarlo(uint64_t n, uint64_t steps, uint64_t ceiling, uint32_t seed) {
        VALECounters t(n, /*with_min_tree=*/true);
        REQUIRE(t.HasMinTree());
        REQUIRE_EQ(t.CountCounters(), n);
        // The leaf count is rounded up to even, so that no node has one leaf
        // child and one internal one; an odd count gets a spare, empty leaf.
        REQUIRE_EQ(t.CountStoredCounters(), 2 * (n + (n & 1)));
        std::vector<uint64_t> ref(n, 0);
        std::mt19937_64 rng(seed);
        CheckMinTree(t, ref);

        for (uint64_t step = 0; step < steps; step++) {
            const uint64_t i = rng() % n;
            switch (rng() % 5) {
                case 0: { const uint64_t v = 1 + rng() % ceiling; t.Set(i, v); ref[i] = v; break; }
                case 1: t.Set(i, 0); ref[i] = 0; break;
                case 2: t.Increment(i); ref[i]++; break;
                case 3: if (ref[i] > 0) { t.Decrement(i); ref[i]--; } break;
                case 4: {
                    const uint64_t j = rng() % n;
                    const uint64_t lo = std::min(i, j), hi = std::max(i, j);
                    if (rng() % 2) {
                        t.ShiftRightAndClear(lo, hi);
                        ShiftRightRef(ref, lo, hi);
                    }
                    else {
                        t.ShiftLeftAndClear(lo, hi);
                        ShiftLeftRef(ref, lo, hi);
                    }
                    break;
                }
            }
            CheckMinTree(t, ref);
        }
    }

    /**
     * The range minimum, against a scan, over every range of a small array and
     * over random ranges of a larger one -- including the whole-array case,
     * which the root answers on its own, and ranges that are empty or all zero.
     */
    static void MinInRangeMonteCarlo(uint64_t n, uint64_t steps, uint64_t ceiling, uint32_t seed) {
        VALECounters t(n, /*with_min_tree=*/true);
        std::vector<uint64_t> ref(n, 0);
        std::mt19937_64 rng(seed);
        const auto check = [&](uint64_t lo, uint64_t hi) {
            uint64_t want = 0;
            for (uint64_t i = lo; i < hi; i++)
                if (ref[i] != 0 && (want == 0 || ref[i] < want))
                    want = ref[i];
            const auto [value, pos] = t.MinInRange(lo, hi);
            REQUIRE_EQ(value, want);
            if (want != 0) {
                REQUIRE_GE(pos, lo);
                REQUIRE_LT(pos, hi);
                REQUIRE_EQ(ref[pos], want);
            }
        };
        for (uint64_t step = 0; step < steps; step++) {
            const uint64_t i = rng() % n;
            const uint64_t v = (rng() % 4 == 0) ? 0 : 1 + rng() % ceiling;
            t.Set(i, v);
            ref[i] = v;
            if (n <= 16) {
                for (uint64_t lo = 0; lo <= n; lo++)
                    for (uint64_t hi = lo; hi <= n; hi++)
                        check(lo, hi);
            }
            else {
                check(0, n);
                for (int r = 0; r < 4; r++) {
                    const uint64_t a = rng() % (n + 1), b = rng() % (n + 1);
                    check(std::min(a, b), std::max(a, b));
                }
            }
        }
    }

    /**
     * Taking an offset off every counter a batch at a time: the two scales the
     * array holds while it is in flight, the rebasing that hides them from a
     * caller moving values about, and the one scale it is left on.
     */
    static void OffsetPassMonteCarlo(uint64_t n, uint64_t batch, uint32_t seed) {
        VALECounters t(n, /*with_min_tree=*/true);
        std::vector<uint64_t> ref(n, 0);
        std::mt19937_64 rng(seed);
        for (uint64_t i = 0; i < n; i++)
            if (rng() % 3 != 0) {
                ref[i] = 1000 + rng() % 5000;
                t.Set(i, ref[i]);
            }

        const uint64_t offset = 999;
        t.BeginOffset(offset);
        REQUIRE(t.OffsetInProgress());
        uint64_t finished = 0;
        while (finished == 0) {
            // Mid-pass: what is below the frontier has had the offset taken
            // off, what is above has not, and `GetRebased` is blind to both.
            const uint64_t frontier = t.OffsetFrontier();
            for (uint64_t i = 0; i < n; i++) {
                const uint64_t applied = (ref[i] != 0 && i < frontier) ? offset : 0;
                REQUIRE_EQ(t.Get(i), ref[i] == 0 ? 0 : ref[i] - applied);
                REQUIRE_EQ(t.GetRebased(i), ref[i]);
                REQUIRE_EQ(t.AppliedOffsetAt(i), i < frontier ? offset : 0);
            }
            // A value written on the rebased scale reads back on it, wherever
            // the frontier happens to be -- this is a cuckoo kick's business.
            const uint64_t moved = rng() % n;
            const uint64_t value = 1000 + rng() % 5000;
            t.SetRebased(moved, value, ref[moved]);
            ref[moved] = value;
            REQUIRE_EQ(t.GetRebased(moved), value);
            finished = t.StepOffset(batch);
        }
        REQUIRE_EQ(finished, offset);
        REQUIRE_FALSE(t.OffsetInProgress());
        for (uint64_t i = 0; i < n; i++)
            REQUIRE_EQ(t.Get(i), ref[i] == 0 ? 0 : ref[i] - offset);
    }

    /** A shift moves the candidate's index, and nothing else about it. */
    static void MinTreeCandidateFollowsItsLeaf() {
        VALECounters t(500, true);
        std::vector<uint64_t> ref(500, 0);
        for (uint64_t i = 100; i < 200; i++) {
            ref[i] = 500 - (i - 100);           // Descending, so the minimum is at 199.
            t.Set(i, ref[i]);
        }
        REQUIRE_EQ(t.MinSlot(), 199);
        REQUIRE_EQ(t.MinValue(), 401);

        t.ShiftRightAndClear(150, 300);         // The candidate is inside the range.
        ShiftRightRef(ref, 150, 300);
        REQUIRE_EQ(t.MinSlot(), 200);           // It moved with its leaf.
        REQUIRE_EQ(t.MinValue(), 401);
        CheckMinTree(t, ref);

        t.ShiftLeftAndClear(120, 250);
        ShiftLeftRef(ref, 120, 250);
        REQUIRE_EQ(t.MinSlot(), 199);
        CheckMinTree(t, ref);

        // Discarding the candidate itself is the one case that has to search.
        const uint64_t at = t.MinSlot();
        t.ShiftLeftAndClear(at, at + 10);
        ShiftLeftRef(ref, at, at + 10);
        CheckMinTree(t, ref);
    }

    /** The tree survives the rebuilds that re-tune the array. */
    static void MinTreeSurvivesRetuning() {
        VALECounters t(2000, true);
        std::vector<uint64_t> ref(2000, 0);
        std::mt19937_64 rng(77);
        for (uint64_t i = 0; i < 2000; i++)
            if (rng() % 3) {
                ref[i] = 100000 + rng() % 400000;
                t.Set(i, ref[i]);
            }
        CheckMinTree(t, ref);

        REQUIRE(t.Retune());                    // A plain re-tuning.
        CheckMinTree(t, ref);

        const uint64_t offset = SmallestNonZero(ref) - 1;
        REQUIRE(t.Retune(offset));              // And one that subtracts an offset.
        for (auto& v : ref)
            if (v != 0)
                v -= offset;
        CheckMinTree(t, ref);

        const VALECounters copy(t);             // A copy carries the tree over.
        CheckMinTree(copy, ref);
    }

    static void ShiftRightRef(std::vector<uint64_t>& ref, uint64_t hole, uint64_t last) {
        for (uint64_t i = last; i > hole; i--)
            ref[i] = ref[i - 1];
        ref[hole] = 0;
    }

    static void ShiftLeftRef(std::vector<uint64_t>& ref, uint64_t hole, uint64_t last) {
        for (uint64_t i = hole; i < last; i++)
            ref[i] = ref[i + 1];
        ref[last] = 0;
    }

    /** Fills the array with a mix of stub-sized, extension-sized, and zero values. */
    static void FillMixed(VALECounters& t, std::vector<uint64_t>& ref, uint64_t seed) {
        std::mt19937_64 rng(seed);
        for (uint64_t i = 0; i < ref.size(); i++) {
            const uint32_t kind = rng() % 4;
            ref[i] = kind == 0 ? 0
                   : kind == 1 ? rng() % 32
                   : kind == 2 ? 32 + rng() % 1000
                               : rng() % (1ULL << 25);
            t.Set(i, ref[i]);
        }
    }

    static void ShiftsWithinAChunk() {
        VALECounters t(500);
        const uint32_t cpc = t.GetCountersPerChunk();
        std::vector<uint64_t> ref(500, 0);
        FillMixed(t, ref, 3);
        CheckAgainst(t, ref);

        // A shift whose whole range sits inside one chunk must leave that
        // chunk's extension pool bit for bit identical: the pool is ordered by
        // the position of the counters its entries belong to, and a shift by
        // one does not disturb that order.
        const uint64_t chunk = 2;
        const uint64_t base = chunk * cpc;
        t.Set(base + 40, 0);                    // The empty slot the shift consumes.
        ref[base + 40] = 0;

        const auto pool_before = Pool(t, chunk);
        t.ShiftRightAndClear(base + 10, base + 40);
        ShiftRightRef(ref, base + 10, base + 40);
        REQUIRE(Pool(t, chunk) == pool_before);
        CheckAgainst(t, ref);

        // ... and the same going the other way.
        const auto pool_before_left = Pool(t, chunk);
        t.Set(base + 10, 0);
        ref[base + 10] = 0;
        const auto pool_after_clear = Pool(t, chunk);
        t.ShiftLeftAndClear(base + 10, base + 40);
        ShiftLeftRef(ref, base + 10, base + 40);
        REQUIRE(Pool(t, chunk) == pool_after_clear);
        CheckAgainst(t, ref);
        (void) pool_before_left;

        // Neighbouring chunks were not touched at all.
        for (uint64_t c = 0; c < t.CountChunks(); c++) {
            if (c == chunk)
                continue;
            for (uint64_t i = c * cpc; i < std::min<uint64_t>((c + 1) * cpc, 500); i++)
                REQUIRE_EQ(t.Get(i), ref[i]);
        }
    }

    static void ShiftsAcrossChunks() {
        VALECounters t(500);
        std::vector<uint64_t> ref(500, 0);
        FillMixed(t, ref, 4);

        // Ranges that span several chunks, so counters cross boundaries and
        // their extensions have to move pool to pool.
        struct Range { uint64_t hole, last; bool right; };
        const std::vector<Range> ranges = {
            {10, 200, true}, {10, 200, false},
            {0, 499, true},  {0, 499, false},
            {67, 69, true},  {67, 69, false},     // Straddling one boundary.
            {135, 137, true}, {135, 137, false},
            {5, 5, true},    {300, 300, false},   // Degenerate: just a clear.
        };
        for (const Range& r : ranges) {
            if (r.right) {
                t.ShiftRightAndClear(r.hole, r.last);
                ShiftRightRef(ref, r.hole, r.last);
            }
            else {
                t.ShiftLeftAndClear(r.hole, r.last);
                ShiftLeftRef(ref, r.hole, r.last);
            }
            CheckAgainst(t, ref);
        }
    }

    /** A chunk holding a tails array shifts along with everything else. */
    static void ShiftsWithTailsArray() {
        VALECounters t(300);
        const uint32_t cpc = t.GetCountersPerChunk();
        std::vector<uint64_t> ref(300, 0);
        // Values big enough to drive the first two chunks into tails arrays.
        for (uint64_t i = 0; i < 2 * cpc; i++) {
            ref[i] = 5000000ULL + 7 * i;
            t.Set(i, ref[i]);
        }
        REQUIRE(HasTails(t, 0));
        REQUIRE(HasTails(t, 1));
        CheckAgainst(t, ref);

        t.Set(cpc / 2, 0);
        ref[cpc / 2] = 0;
        t.ShiftRightAndClear(3, cpc / 2);
        ShiftRightRef(ref, 3, cpc / 2);
        CheckAgainst(t, ref);

        // A shift that crosses out of a chunk with a tails array and into
        // another one.
        t.Set(2 * cpc - 1, 0);
        ref[2 * cpc - 1] = 0;
        t.ShiftRightAndClear(5, 2 * cpc - 1);
        ShiftRightRef(ref, 5, 2 * cpc - 1);
        CheckAgainst(t, ref);

        t.ShiftLeftAndClear(7, 2 * cpc + 10);
        ShiftLeftRef(ref, 7, 2 * cpc + 10);
        CheckAgainst(t, ref);
    }

    /** Random values, random shifts, checked against a plain vector throughout. */
    static void ShiftMonteCarlo() {
        std::mt19937_64 rng(5);
        for (const uint64_t n : {70ULL, 137ULL, 500ULL, 1024ULL}) {
            VALECounters t(n);
            std::vector<uint64_t> ref(n, 0);
            FillMixed(t, ref, n);

            for (int32_t step = 0; step < 400; step++) {
                const uint64_t a = rng() % n, b = rng() % n;
                const uint64_t hole = std::min(a, b), last = std::max(a, b);
                if (rng() % 2) {
                    t.ShiftRightAndClear(hole, last);
                    ShiftRightRef(ref, hole, last);
                }
                else {
                    t.ShiftLeftAndClear(hole, last);
                    ShiftLeftRef(ref, hole, last);
                }
                // Keep feeding in fresh values so the pools stay interesting.
                for (int32_t k = 0; k < 4; k++) {
                    const uint64_t i = rng() % n;
                    ref[i] = (rng() % 3) ? rng() % (1ULL << 22) : 0;
                    t.Set(i, ref[i]);
                }
                const uint64_t i = rng() % n;
                t.Increment(i);
                ref[i]++;
                CheckAgainst(t, ref);
            }
        }
    }

    /*
     * ------------------------------------------------------------------
     * Tuning.
     * ------------------------------------------------------------------
     */

    /** The default tuning, and the shape it implies. */
    static void DefaultTuning() {
        const auto [cpc, ss] = VALECounters::tune_params(nullptr);
        REQUIRE_EQ(cpc, VALECounters::default_counter_per_cache_line);
        REQUIRE_EQ(ss, VALECounters::default_stub_size);

        // A histogram of nothing but tiny values wants short stubs and many
        // counters per chunk; one of nothing but large values wants the
        // opposite. Either way the chunk still has to fit in a cache line.
        uint32_t small_hist[65] = {}, large_hist[65] = {};
        small_hist[1] = 100000;
        large_hist[24] = 100000;
        const auto [small_cpc, small_ss] = VALECounters::tune_params(small_hist);
        const auto [large_cpc, large_ss] = VALECounters::tune_params(large_hist);
        REQUIRE_GE(small_ss, VALECounters::min_stub_size);
        REQUIRE_GT(large_ss, small_ss);
        REQUIRE_LT(large_cpc, small_cpc);
        for (const auto& [c, s] : {std::pair{small_cpc, small_ss}, std::pair{large_cpc, large_ss}}) {
            REQUIRE_LE(c * (s + 1), VALECounters::cache_line_size - 1 - 2 * 24);
            REQUIRE_GE(c, VALECounters::min_counter_per_cache_line);
            REQUIRE_LE(c, VALECounters::max_counter_per_cache_line);
        }
    }

    /**
     * When enough chunks have spilled into tails arrays, retuning re-derives
     * VALE's parameters from the counters themselves and rebuilds under them,
     * losing nothing and reclaiming the tails arrays.
     */
    static void RetunesAwayFromTailsArrays() {
        VALECounters t(20000);
        std::vector<uint64_t> ref(20000, 0);
        std::mt19937_64 rng(6);

        // Counts far too large for a five-bit stub, which is what the default
        // tuning starts with.
        const uint32_t initial_cpc = t.GetCountersPerChunk();
        const uint32_t initial_stub = t.GetStubLength();
        for (uint64_t i = 0; i < 20000; i++) {
            ref[i] = 100000 + rng() % 1000000;
            t.Set(i, ref[i]);
        }
        REQUIRE(t.ShouldRetune());
        const uint64_t size_before = t.SizeInBytes();
        const uint32_t tails_before = t.CountChunksWithTails();
        REQUIRE_GT(tails_before, 0);

        REQUIRE(t.MaybeRetune());
        CheckAgainst(t, ref);
        // A longer stub for longer counts, and correspondingly fewer of them
        // per chunk.
        REQUIRE_GT(t.GetStubLength(), initial_stub);
        REQUIRE_LT(t.GetCountersPerChunk(), initial_cpc);
        // The point of the exercise: the tails arrays are gone, and with them
        // the memory they cost.
        REQUIRE_LT(t.CountChunksWithTails(), tails_before);
        REQUIRE_LT(t.SizeInBytes(), size_before);
        REQUIRE_FALSE(t.ShouldRetune());

        // The rebuilt array works exactly as before.
        for (uint64_t i = 0; i < 20000; i += 3) {
            t.Increment(i);
            ref[i]++;
        }
        CheckAgainst(t, ref);
        t.ShiftRightAndClear(100, 5000);
        ShiftRightRef(ref, 100, 5000);
        CheckAgainst(t, ref);
    }

    /**
     * `MaybeRetune` only fires when counters outgrow their chunk, so something
     * else has to notice when they shrink -- which is what a Misra-Gries
     * decrement sweep does to all of them at once. `RetuneIfNarrower` is that
     * direction, and it only pays out when the stub really can get narrower.
     */
    static void RetunesWhenCountersShrink() {
        VALECounters t(20000);
        std::vector<uint64_t> ref(20000, 0);
        std::mt19937_64 rng(13);
        for (uint64_t i = 0; i < 20000; i++) {
            ref[i] = (i % 4 == 0) ? 0 : 90000 + rng() % 3000;
            t.Set(i, ref[i]);
        }
        REQUIRE(t.Retune());
        const uint32_t wide_stub = t.GetStubLength();
        const uint32_t wide_per_chunk = t.GetCountersPerChunk();
        const uint64_t wide_bytes = t.SizeInBytes();

        // Tuned to what it holds, there is nothing to win by asking again.
        REQUIRE_FALSE(t.RetuneIfNarrower());
        REQUIRE_EQ(t.GetStubLength(), wide_stub);

        // Now bring every counter down, as a run of decrement sweeps would.
        for (uint64_t i = 0; i < 20000; i++) {
            if (ref[i] == 0)
                continue;
            const uint64_t left = 1 + rng() % 7;
            for (uint64_t k = ref[i]; k > left; k--)
                t.Decrement(i);
            ref[i] = left;
        }
        CheckAgainst(t, ref);

        // The tuning follows them down: a shorter stub, more counters per
        // chunk, and a smaller array, with every value intact.
        REQUIRE(t.RetuneIfNarrower());
        CheckAgainst(t, ref);
        REQUIRE_LT(t.GetStubLength(), wide_stub);
        REQUIRE_GT(t.GetCountersPerChunk(), wide_per_chunk);
        REQUIRE_LT(t.SizeInBytes(), wide_bytes);
        REQUIRE_EQ(t.CountChunksWithTails(), 0);

        // And once it has followed them, asking again wins nothing.
        REQUIRE_FALSE(t.RetuneIfNarrower());

        // The array works normally afterwards.
        t.Increment(7);
        ref[7]++;
        t.ShiftRightAndClear(10, 400);
        ShiftRightRef(ref, 10, 400);
        CheckAgainst(t, ref);
    }

    /** Retuning never spins: it stops once it has nothing better to offer. */
    static void RetuningTerminates() {
        VALECounters t(5000);
        std::mt19937_64 rng(7);
        std::vector<uint64_t> ref(5000, 0);
        // Values so large that no tuning avoids tails arrays entirely, but
        // still inside the range a 32-bit tail can hold.
        for (uint64_t i = 0; i < 5000; i++) {
            ref[i] = (1ULL << 30) + rng() % (1ULL << 30);
            t.Set(i, ref[i]);
        }
        int32_t retunes = 0;
        while (t.MaybeRetune()) {
            CheckAgainst(t, ref);
            retunes++;
            REQUIRE_LT(retunes, 100);   // Must converge, not oscillate.
        }
        REQUIRE_FALSE(t.ShouldRetune());
        CheckAgainst(t, ref);
    }

    static void CopyMoveAndReset() {
        VALECounters t(3000);
        std::vector<uint64_t> ref(3000, 0);
        FillMixed(t, ref, 8);
        // Push a few chunks onto tails arrays, so the copy has to rebuild them
        // rather than share the pointers.
        for (uint64_t i = 0; i < 100; i++) {
            ref[i] = 10000000ULL + i;
            t.Set(i, ref[i]);
        }
        REQUIRE_GT(t.CountChunksWithTails(), 0);

        VALECounters copy(t);
        CheckAgainst(copy, ref);
        // The copy owns its own tails arrays: writing through one must not be
        // visible through the other.
        copy.Set(0, 12345678);
        REQUIRE_EQ(t.Get(0), ref[0]);
        copy.Set(0, ref[0]);
        REQUIRE(Pool(copy, 0) != Pool(t, 0));   // Different pointers in the pool.

        VALECounters moved(std::move(copy));
        CheckAgainst(moved, ref);

        VALECounters assigned(4);
        assigned = t;
        CheckAgainst(assigned, ref);
        REQUIRE_EQ(assigned.GetCountersPerChunk(), t.GetCountersPerChunk());

        t.Reset();
        REQUIRE_EQ(t.CountChunksWithTails(), 0);
        std::vector<uint64_t> zeros(3000, 0);
        CheckAgainst(t, zeros);
        CheckAgainst(assigned, ref);

        // The array works again after a reset.
        FillMixed(t, ref, 9);
        CheckAgainst(t, ref);
    }

    /** Sizes that do not fill their last chunk. */
    static void PartialLastChunk() {
        for (const uint64_t n : {1ULL, 2ULL, 67ULL, 68ULL, 69ULL, 137ULL}) {
            VALECounters t(n);
            std::vector<uint64_t> ref(n, 0);
            FillMixed(t, ref, n + 100);
            CheckAgainst(t, ref);
            if (n < 2)
                continue;
            t.ShiftRightAndClear(0, n - 1);
            ShiftRightRef(ref, 0, n - 1);
            CheckAgainst(t, ref);
            t.ShiftLeftAndClear(0, n - 1);
            ShiftLeftRef(ref, 0, n - 1);
            CheckAgainst(t, ref);
        }
    }
};

}   // namespace sublime

using sublime::VALECounters;
using sublime::VALECountersTest;

TEST_SUITE("VALECounters") {
    TEST_CASE("get and set") {
        VALECountersTest::GetAndSet();
    }

    TEST_CASE("increment and decrement") {
        VALECountersTest::IncrementAndDecrement();
    }

    TEST_CASE("spills into a tails array") {
        VALECountersTest::SpillsIntoTailsArray();
    }

    TEST_CASE("shifts within a chunk leave the pool alone") {
        VALECountersTest::ShiftsWithinAChunk();
    }

    TEST_CASE("shifts across chunks") {
        VALECountersTest::ShiftsAcrossChunks();
    }

    TEST_CASE("shifts with a tails array") {
        VALECountersTest::ShiftsWithTailsArray();
    }

    TEST_CASE("shift monte carlo") {
        VALECountersTest::ShiftMonteCarlo();
    }

    TEST_CASE("default tuning") {
        VALECountersTest::DefaultTuning();
    }

    TEST_CASE("retunes away from tails arrays") {
        VALECountersTest::RetunesAwayFromTailsArrays();
    }

    TEST_CASE("retunes when counters shrink") {
        VALECountersTest::RetunesWhenCountersShrink();
    }

    TEST_CASE("retuning terminates") {
        VALECountersTest::RetuningTerminates();
    }

    TEST_CASE("copy, move, and reset") {
        VALECountersTest::CopyMoveAndReset();
    }

    TEST_CASE("pool shifts keep what is below them") {
        VALECountersTest::PoolShiftKeepsWhatIsBelowIt();
        VALECountersTest::PoolShiftUpKeepsWhatIsBelowIt();
    }

    TEST_CASE("min tree monte carlo") {
        VALECountersTest::MinTreeMonteCarlo(/*n=*/1, /*steps=*/200, /*ceiling=*/50, 1);
        VALECountersTest::MinTreeMonteCarlo(2, 2000, 50, 2);
        VALECountersTest::MinTreeMonteCarlo(7, 20000, 5000, 3);
        VALECountersTest::MinTreeMonteCarlo(64, 20000, 5000, 4);
        VALECountersTest::MinTreeMonteCarlo(777, 20000, 4000000000ULL, 5);
        VALECountersTest::MinTreeMonteCarlo(1000, 20000, 5000, 6);
    }

    TEST_CASE("min tree range minimum") {
        VALECountersTest::MinInRangeMonteCarlo(/*n=*/2, /*steps=*/200, /*ceiling=*/50, 51);
        VALECountersTest::MinInRangeMonteCarlo(7, 300, 5000, 52);
        VALECountersTest::MinInRangeMonteCarlo(16, 300, 5000, 53);
        VALECountersTest::MinInRangeMonteCarlo(777, 3000, 4000000000ULL, 54);
        VALECountersTest::MinInRangeMonteCarlo(1024, 3000, 5000, 55);
    }

    TEST_CASE("an offset merged out a batch at a time") {
        VALECountersTest::OffsetPassMonteCarlo(/*n=*/64, /*batch=*/8, 61);
        VALECountersTest::OffsetPassMonteCarlo(777, 16, 62);
        VALECountersTest::OffsetPassMonteCarlo(1000, 1, 63);
        VALECountersTest::OffsetPassMonteCarlo(1000, 4096, 64);
    }

    TEST_CASE("min tree candidate follows its leaf") {
        VALECountersTest::MinTreeCandidateFollowsItsLeaf();
    }

    TEST_CASE("min tree survives retuning") {
        VALECountersTest::MinTreeSurvivesRetuning();
    }

    TEST_CASE("partial last chunk") {
        VALECountersTest::PartialLastChunk();
    }
}
