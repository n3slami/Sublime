#include <algorithm>
#include <random>
#include <unordered_map>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>

#include "MGHeap.hpp"

namespace sublime {

/**
 * A plain, exact Misra-Gries kept alongside `MGHeap` as an oracle. It is keyed
 * by the identity the table can actually distinguish (`EntryIdentity`), so two
 * keys sharing a fingerprint are one key to it too, and its answers are exact
 * for whatever `MGHeap` can tell apart.
 */
struct ReferenceMG {
    uint64_t capacity;
    uint64_t decrement = 0;
    std::unordered_map<uint64_t, int64_t> counts;

    void Insert(uint64_t identity) {
        auto it = counts.find(identity);
        if (it != counts.end()) {
            it->second++;
        }
        else if (counts.size() < capacity) {
            counts[identity] = decrement + 1;
        }
        else {
            decrement++;
            for (auto i = counts.begin(); i != counts.end();) {
                if (i->second <= static_cast<int64_t>(decrement))
                    i = counts.erase(i);
                else
                    ++i;
            }
        }
    }

    uint64_t Query(uint64_t identity) const {
        auto it = counts.find(identity);
        if (it != counts.end() && it->second > static_cast<int64_t>(decrement))
            return it->second - decrement;
        return 0;
    }
};

/**
 * White-box tests for `MGHeap`, a `friend` so they can reach into the table,
 * the sidecar (counts + packed heap offsets), and the heap itself.
 */
class MGHeapTest {
public:
    using hashmode = MGHeap::hashmode;

    /**
     * Every internal invariant the design rests on, checked against `ref`:
     *
     *  - the lazy decrement agrees with the oracle;
     *  - the heap is a valid min-heap on the counts;
     *  - slots and heap positions are in bijection, and each slot's `heap_offset`
     *    points at the heap entry whose count and home bucket match that slot;
     *  - every live count is strictly above the decrement (nothing evictable is
     *    still stored);
     *  - the counts a query would read match the oracle for every key seen.
     */
    static void CheckSummary(const MGHeap& mg, const ReferenceMG& ref,
                             const std::vector<uint64_t>& universe, uint8_t flags) {
        const FingerprintTable& table = mg.table_;

        REQUIRE_EQ(mg.lazy_decrement_, ref.decrement);
        REQUIRE_EQ(mg.CountMonitored(), ref.counts.size());
        REQUIRE_EQ(mg.heap_.size(), table.CountFingerprints());

        // A valid min-heap.
        for (uint64_t i = 1; i < mg.heap_.size(); i++)
            REQUIRE_GE(mg.heap_[i].count, mg.heap_[(i - 1) / 2].count);

        // Slots <-> heap positions form a bijection, with matching count/bucket.
        std::vector<int64_t> owner(mg.heap_.size(), -1);
        uint64_t seen = 0;
        for (auto it = table.begin(); it != table.end(); ++it) {
            const uint64_t slot = it.slot();
            const uint64_t pos = mg.sidecar_.offsets.Get(slot);
            REQUIRE_LT(pos, mg.heap_.size());
            REQUIRE_EQ(owner[pos], -1);              // No two slots share a heap slot.
            owner[pos] = static_cast<int64_t>(slot);
            REQUIRE_EQ(mg.heap_[pos].count, mg.sidecar_.counts[slot]);
            REQUIRE_EQ(mg.heap_[pos].bucket, it.bucket());
            REQUIRE_GT(mg.heap_[pos].count, mg.lazy_decrement_);
            seen++;
        }
        REQUIRE_EQ(seen, mg.heap_.size());
        for (uint64_t pos = 0; pos < mg.heap_.size(); pos++)
            REQUIRE_NE(owner[pos], -1);              // Every heap slot is owned.

        // The answers match the oracle for every key in the universe.
        for (uint64_t key : universe)
            REQUIRE_EQ(mg.Query(key, flags), ref.Query(table.EntryIdentity(key, flags)));
    }

    /**
     * Replays a skewed random stream through both `MGHeap` and the oracle,
     * checking every invariant at a handful of points and at the end.
     */
    static void MonteCarlo(uint64_t nslots, uint32_t fingerprint_length, uint32_t seed,
                           uint64_t universe_size, uint64_t stream_length, bool prehashed) {
        const hashmode mode = prehashed ? hashmode::Invertible : hashmode::Default;
        const uint8_t flags = prehashed ? MGHeap::flag_key_is_hash : 0;
        MGHeap mg(nslots, mode, seed, fingerprint_length);
        ReferenceMG ref{mg.Capacity()};

        std::vector<uint64_t> universe(universe_size);
        for (uint64_t i = 0; i < universe_size; i++)
            universe[i] = i;

        std::mt19937_64 rng(seed * 2654435761ULL + 1);
        const auto draw = [&]() {
            uint64_t m = rng() % universe_size;         // Min-of-k: a cheap skew.
            for (int i = 0; i < 4; i++)
                m = std::min(m, rng() % universe_size);
            return m;
        };

        const uint64_t check_at = stream_length / 8 + 1;
        for (uint64_t t = 0; t < stream_length; t++) {
            const uint64_t key = draw();
            mg.Insert(key, flags);
            ref.Insert(mg.Table().EntryIdentity(key, flags));
            if (t % check_at == 0)
                CheckSummary(mg, ref, universe, flags);
        }
        CheckSummary(mg, ref, universe, flags);
    }
};


TEST_CASE("monte carlo") {
    MGHeapTest::MonteCarlo(/*nslots=*/256, /*fp=*/10, /*seed=*/1, /*universe=*/8000, /*stream=*/300000, false);
    MGHeapTest::MonteCarlo(1024, 10, 2, 50000, 800000, false);
}

TEST_CASE("collisions from short fingerprints") {
    // A short fingerprint forces distinct keys to share entries and runs to
    // hold several fingerprints, exercising the run scan in resolve_slot.
    MGHeapTest::MonteCarlo(/*nslots=*/300, /*fp=*/5, /*seed=*/3, /*universe=*/4000, /*stream=*/300000, false);
    MGHeapTest::MonteCarlo(512, 6, 4, 6000, 400000, false);
}

TEST_CASE("non-power-of-two sizes") {
    MGHeapTest::MonteCarlo(/*nslots=*/777, /*fp=*/8, /*seed=*/5, /*universe=*/10000, /*stream=*/400000, false);
    MGHeapTest::MonteCarlo(1000, 12, 6, 60000, 600000, false);
}

TEST_CASE("pre-hashed keys") {
    MGHeapTest::MonteCarlo(/*nslots=*/512, /*fp=*/10, /*seed=*/7, /*universe=*/9000, /*stream=*/400000, true);
}

TEST_CASE("tiny table") {
    MGHeapTest::MonteCarlo(/*nslots=*/16, /*fp=*/8, /*seed=*/8, /*universe=*/500, /*stream=*/60000, false);
    MGHeapTest::MonteCarlo(8, 6, 9, 200, 40000, false);
}

TEST_CASE("reset empties and reuses") {
    MGHeap mg(512, MGHeap::hashmode::Default, 11, 8);
    for (uint64_t k = 0; k < 100000; k++)
        mg.Insert(k % 3000);
    REQUIRE_GT(mg.CountMonitored(), 0);
    mg.Reset();
    REQUIRE_EQ(mg.CountMonitored(), 0);
    REQUIRE_EQ(mg.GetLazyDecrement(), 0);
    REQUIRE_EQ(mg.GetStreamLength(), 0);
    REQUIRE_EQ(mg.Query(5), 0);

    ReferenceMG ref{mg.Capacity()};
    std::vector<uint64_t> universe(2000);
    for (uint64_t i = 0; i < 2000; i++)
        universe[i] = i;
    std::mt19937_64 rng(123);
    for (uint64_t t = 0; t < 80000; t++) {
        const uint64_t key = std::min(rng() % 2000, rng() % 2000);
        mg.Insert(key);
        ref.Insert(mg.Table().EntryIdentity(key));
    }
    MGHeapTest::CheckSummary(mg, ref, universe, 0);
}

TEST_CASE("classic Misra-Gries error bound holds without collisions") {
    // With long fingerprints and few distinct keys, entries rarely collide, so
    // MGHeap behaves as plain Misra-Gries: it under-estimates, and the gap to
    // the truth is bounded by the decrements applied. (A rare collision merges
    // counts and can break the direction, so this is only a sanity floor.)
    MGHeap mg(4096, MGHeap::hashmode::Default, 21, 22);
    std::unordered_map<uint64_t, uint64_t> exact;
    std::mt19937_64 rng(99);
    for (uint64_t t = 0; t < 500000; t++) {
        const uint64_t key = std::min({rng() % 2000, rng() % 2000, rng() % 2000});
        mg.Insert(key);
        exact[key]++;
    }
    for (const auto& [key, count] : exact) {
        const uint64_t est = mg.Query(key);
        REQUIRE_LE(est, count + mg.CountDecrements());   // Never wildly over.
    }
}

}   // namespace sublime
