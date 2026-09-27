#include <algorithm>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>

#include "CuckooTable.hpp"
#include "MG.hpp"

namespace sublime {

/**
 * A plain, exact Misra-Gries kept alongside `MG` as an oracle. It is keyed by
 * the identity the table can actually distinguish (`EntryIdentity`), so two
 * keys sharing a fingerprint are one key to it too, and its answers are exact
 * for whatever `MG` can tell apart.
 *
 * It decrements eagerly, entry by entry, exactly as `MG` now does -- no lazy
 * decrement -- and hands the occurrence that paid for a decrement one of the
 * slots that decrement emptied.
 */
struct ReferenceMG {
    uint64_t capacity;
    uint64_t total_decrements = 0;
    std::unordered_map<uint64_t, uint64_t> counts;

    void Insert(uint64_t identity) {
        auto it = counts.find(identity);
        if (it != counts.end()) {
            it->second++;
            return;
        }
        if (counts.size() < capacity) {
            counts[identity] = 1;
            return;
        }
        total_decrements++;
        uint64_t freed = 0;
        for (auto i = counts.begin(); i != counts.end();) {
            if (--i->second == 0) {
                i = counts.erase(i);
                freed++;
            }
            else {
                ++i;
            }
        }
        if (freed > 0)
            counts[identity] = 1;
    }

    uint64_t Query(uint64_t identity) const {
        auto it = counts.find(identity);
        return it == counts.end() ? 0 : it->second;
    }
};

/**
 * White-box tests for `MG`, a `friend` so they can reach into the table and
 * the sidecar holding the counts.
 */
class MGTest {
public:
    using hashmode = MG<>::hashmode;

    /**
     * Every internal invariant the design rests on, checked against `ref`:
     *
     *  - the summary monitors exactly the keys the oracle does;
     *  - every stored count is strictly positive (an emptied entry is evicted,
     *    never left behind);
     *  - the decrements applied agree with the oracle's;
     *  - the counts a query would read match the oracle for every key seen.
     */
    template <typename MGT>
    static void CheckSummary(const MGT& mg, const ReferenceMG& ref,
                             const std::vector<uint64_t>& universe, uint8_t flags) {
        const auto& table = mg.table_;

        REQUIRE_EQ(mg.CountMonitored(), ref.counts.size());
        REQUIRE_EQ(mg.CountDecrements(), ref.total_decrements);

        uint64_t seen = 0;
        for (auto it = table.begin(); it != table.end(); ++it) {
            REQUIRE_GT(mg.sidecar_.counts[it.slot()], 0);
            seen++;
        }
        REQUIRE_EQ(seen, table.CountFingerprints());

        // The answers match the oracle for every key in the universe.
        for (uint64_t key : universe)
            REQUIRE_EQ(mg.Query(key, flags), ref.Query(table.EntryIdentity(key, flags)));
    }

    /** The invariants that hold whatever the table has dropped. */
    template <typename MGT>
    static void CheckStructure(const MGT& mg, const std::map<uint64_t, uint64_t>& exact,
                               const std::vector<uint64_t>& universe, uint8_t flags) {
        const auto& table = mg.GetTable();
        REQUIRE_LE(mg.CountMonitored(), mg.Capacity());

        uint64_t seen = 0, mass = 0;
        for (auto it = table.begin(); it != table.end(); ++it) {
            REQUIRE_GT(mg.sidecar_.counts[it.slot()], 0);
            mass += mg.sidecar_.counts[it.slot()];
            seen++;
        }
        REQUIRE_EQ(seen, table.CountFingerprints());
        REQUIRE_LE(mass, mg.GetStreamLength());

        for (uint64_t key : universe) {
            REQUIRE_LE(table.Count(key, flags), 1);     // One entry per key, or none.
            const auto at = exact.find(table.EntryIdentity(key, flags));
            REQUIRE_LE(mg.Query(key, flags), at == exact.end() ? 0 : at->second);
        }
    }

    /**
     * Replays a skewed random stream through both `MG` and the oracle.
     *
     * The oracle is exact, and it can only be followed until the table first
     * drops something. A cuckoo filter gives up on a kick path now and then --
     * it does at the load factor Misra-Gries runs its table at -- and either
     * drops whichever entry the kicks were carrying or turns the arrival away.
     * Either one leaves the summary with room the oracle does not have, so from
     * that moment the two take different decisions and no exact comparison
     * means anything: the summary can even hold *more* keys than the oracle,
     * having decremented fewer times.
     *
     * So the oracle is checked in lockstep up to the first loss, on a fine
     * period -- the window is only a few percent of the stream in the tightest
     * of these configurations, and a coarse period would leave the comparison
     * happening once, at the empty summary -- and what is checked after it are
     * the invariants no loss can break: a key is stored once or not at all, no
     * count is zero, and an estimate never exceeds the true frequency of the
     * keys sharing its entry, which losing counts cannot violate.
     */
    static void MonteCarlo(uint64_t nslots, uint32_t fingerprint_length, uint32_t seed,
                           uint64_t universe_size, uint64_t stream_length, bool prehashed,
                           uint64_t min_checks = 8) {
        const hashmode mode = prehashed ? hashmode::Invertible : hashmode::Default;
        const uint8_t flags = prehashed ? MG<>::flag_key_is_hash : 0;
        MG<> mg(nslots, mode, seed, fingerprint_length);
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

        std::map<uint64_t, uint64_t> exact;             // Identity -> true frequency.
        bool oracle_follows = true;
        uint64_t checks = 0;
        // A table of a few buckets loses an entry almost at once, so there the
        // window is a handful of insertions and every one of them is checked.
        const uint64_t check_at = nslots <= 64 ? 1 : 500;
        for (uint64_t t = 0; t < stream_length; t++) {
            const uint64_t key = draw();
            const int32_t status = mg.Insert(key, flags);
            const uint64_t identity = mg.GetTable().EntryIdentity(key, flags);
            ref.Insert(identity);
            exact[identity]++;
            if (!oracle_follows)
                continue;
            oracle_follows = status == 0 && mg.GetTable().CountLostEntries() == 0;
            if (oracle_follows && t % check_at == 0) {
                CheckSummary(mg, ref, universe, flags);
                checks++;
            }
        }
        REQUIRE_GE(checks, min_checks);                 // The window was not vacuous.
        REQUIRE_GT(mg.CountDecrements(), 0);            // And the run reached case 3.
        CheckStructure(mg, exact, universe, flags);
    }
};


TEST_CASE("monte carlo") {
    MGTest::MonteCarlo(/*nslots=*/256, /*fp=*/10, /*seed=*/1, /*universe=*/8000, /*stream=*/300000, false);
    MGTest::MonteCarlo(1024, 10, 2, 50000, 800000, false);
}

TEST_CASE("more seeds and shapes") {
    MGTest::MonteCarlo(/*nslots=*/256, /*fp=*/10, /*seed=*/41, /*universe=*/8000, /*stream=*/300000, false);
    MGTest::MonteCarlo(1024, 10, 42, 50000, 800000, false);
    MGTest::MonteCarlo(512, 12, 43, 9000, 400000, false);
}

TEST_CASE("collisions from short fingerprints") {
    // A short fingerprint forces distinct keys to share entries, and a bucket
    // to hold several matching fingerprints, exercising the bucket scan.
    MGTest::MonteCarlo(/*nslots=*/300, /*fp=*/5, /*seed=*/3, /*universe=*/4000, /*stream=*/300000, false);
    MGTest::MonteCarlo(512, 6, 4, 6000, 400000, false);
}

TEST_CASE("non-power-of-two sizes") {
    MGTest::MonteCarlo(/*nslots=*/777, /*fp=*/8, /*seed=*/5, /*universe=*/10000, /*stream=*/400000, false);
    MGTest::MonteCarlo(1000, 12, 6, 60000, 600000, false);
}

TEST_CASE("pre-hashed keys") {
    MGTest::MonteCarlo(/*nslots=*/512, /*fp=*/10, /*seed=*/7, /*universe=*/9000, /*stream=*/400000, true);
}

TEST_CASE("tiny table") {
    // Four buckets of four slots: the kick paths have nowhere to go, so the
    // oracle is good for only the first few insertions and the rest of the run
    // rests on the structural invariants.
    MGTest::MonteCarlo(/*nslots=*/16, /*fp=*/8, /*seed=*/8, /*universe=*/500,
                       /*stream=*/60000, false, /*min_checks=*/1);
    MGTest::MonteCarlo(8, 6, 9, 200, 40000, false, 1);
}

TEST_CASE("a decrement sweep can empty several entries at once") {
    // Everything monitored sits at exactly one, so the first key that finds the
    // summary full empties every one of them, and takes one of the slots.
    MG<> mg(64, MG<>::hashmode::Invertible, 13, 20);
    uint64_t admitted = 0;
    for (uint64_t k = 0; admitted < mg.Capacity(); k++) {
        if (mg.IsMonitored(k, MG<>::flag_key_is_hash))
            continue;               // Sharing an entry would leave it above one.
        mg.Insert(k, MG<>::flag_key_is_hash);
        admitted = mg.CountMonitored();
    }
    REQUIRE_EQ(mg.CountMonitored(), mg.Capacity());
    const uint64_t full = mg.CountMonitored();

    mg.Insert(~0ULL, MG<>::flag_key_is_hash);
    REQUIRE_EQ(mg.CountDecrements(), 1);             // One sweep, one off each entry.
    REQUIRE_EQ(mg.CountMonitored(), 1);              // All emptied, one admitted.
    REQUIRE_EQ(mg.Query(~0ULL, MG<>::flag_key_is_hash), 1);
}

TEST_CASE("an occurrence that frees nothing is dropped") {
    // Give one key a head start so nothing reaches zero on the first sweep.
    MG<> mg(64, MG<>::hashmode::Invertible, 17, 20);
    uint64_t admitted = 0;
    for (uint64_t k = 0; admitted < mg.Capacity(); k++) {
        if (mg.IsMonitored(k, MG<>::flag_key_is_hash))
            continue;               // One key per entry, so every count is three.
        for (int rep = 0; rep < 3; rep++)
            mg.Insert(k, MG<>::flag_key_is_hash);
        admitted = mg.CountMonitored();
    }
    const uint64_t full = mg.CountMonitored();

    mg.Insert(~0ULL, MG<>::flag_key_is_hash);
    REQUIRE_EQ(mg.CountMonitored(), full);           // Nothing emptied...
    REQUIRE_EQ(mg.Query(~0ULL, MG<>::flag_key_is_hash), 0);   // ...so nothing admitted.
}

TEST_CASE("reset empties and reuses") {
    MG<> mg(512, MG<>::hashmode::Default, 11, 8);
    for (uint64_t k = 0; k < 100000; k++)
        mg.Insert(k % 3000);
    REQUIRE_GT(mg.CountMonitored(), 0);
    mg.Reset();
    REQUIRE_EQ(mg.CountMonitored(), 0);
    REQUIRE_EQ(mg.CountDecrements(), 0);
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
        ref.Insert(mg.GetTable().EntryIdentity(key));
    }
    MGTest::CheckSummary(mg, ref, universe, 0);
}

TEST_CASE("classic Misra-Gries error bound holds without collisions") {
    // With long fingerprints and few distinct keys, entries rarely collide, so
    // MG behaves as plain Misra-Gries: it under-estimates, and the gap to the
    // truth is bounded by the decrements each key took, which is the total
    // decrements divided by the capacity.
    MG<> mg(4096, MG<>::hashmode::Default, 21, 22);
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
        if (est < count)
            REQUIRE_LE(count - est, mg.CountDecrements());   // The Misra-Gries guarantee.
    }
    // A sweep takes one off every monitored count, so the stream has to have
    // been long enough to have put that much in.
    REQUIRE_LE(mg.CountDecrements() * mg.Capacity(), mg.GetStreamLength());
}

}   // namespace sublime
