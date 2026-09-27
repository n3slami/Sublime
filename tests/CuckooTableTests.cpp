#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>

#include "CuckooTable.hpp"

namespace sublime {

/** White-box tests for `CuckooTable`. */
class CuckooTableTest {
public:
    using hashmode = CuckooTable::hashmode;

    /**
     * What has to hold of the table between operations: every occupied slot
     * sits in one of its entry's two buckets, its flag says which, and the
     * entry count agrees with what is actually stored.
     */
    static void CheckStructure(const CuckooTable& t) {
        uint64_t stored = 0;
        for (uint64_t slot = 0; slot < t.CountSlots(); slot++) {
            const uint64_t raw = t.read_slot(slot);
            if (t.fingerprint_of(raw) == 0)
                continue;
            stored++;
            const uint64_t bucket = slot / t.GetBucketDepth();
            REQUIRE_EQ(t.BucketOfSlot(slot), bucket);
            // The hash the flag lets us recover has to land back here.
            const uint64_t hash = t.hash_of(bucket, raw);
            const uint64_t primary = t.bucket_from_hash(hash);
            const uint64_t other = t.alternate(primary, t.fingerprint_of(raw));
            REQUIRE((bucket == primary || bucket == other));
            REQUIRE_EQ(bucket, t.flag_of(raw) ? other : primary);
            REQUIRE_EQ(t.fingerprint_from_hash(hash), t.fingerprint_of(raw));
        }
        REQUIRE_EQ(stored, t.CountFingerprints());
    }

    /** Fills a table to its load factor and checks nothing was lost. */
    static void FillsToItsLoadFactor(uint64_t nslots, uint64_t key_bits, uint32_t seed) {
        CuckooTable t(nslots, key_bits, hashmode::Default, seed);
        std::mt19937_64 rng(seed);
        std::vector<uint64_t> in;
        while (t.LoadFactor() < CuckooTable::max_load_factor) {
            const uint64_t key = rng();
            if (t.Insert(key) < 0)
                break;
            in.push_back(key);
        }
        CheckStructure(t);
        REQUIRE_GE(t.LoadFactor(), 0.9);            // A cuckoo filter should get there.
        REQUIRE_EQ(t.CountLostEntries(), 0);
        for (const uint64_t key : in)
            REQUIRE(t.Contains(key));               // No false negatives, ever.
    }

    /** Inserts and deletes at random, against an exact multiset. */
    static void InsertDeleteMonteCarlo(uint64_t nslots, uint32_t seed) {
        CuckooTable t(nslots, 26, hashmode::Default, seed);
        std::multiset<uint64_t> reference;
        std::vector<uint64_t> live;
        std::mt19937_64 rng(seed + 1);

        for (int32_t step = 0; step < 100000; step++) {
            if ((rng() % 100) < 60 && t.LoadFactor() < 0.9) {
                const uint64_t key = rng() % 100000;
                if (t.Insert(key) == 0) {
                    reference.insert(key);
                    live.push_back(key);
                }
            }
            else if (!live.empty()) {
                const size_t at = rng() % live.size();
                const uint64_t key = live[at];
                live[at] = live.back();
                live.pop_back();
                REQUIRE_EQ(t.Delete(key), 0);
                reference.erase(reference.find(key));
            }
            if (step % 2000 == 0) {
                CheckStructure(t);
                REQUIRE_EQ(t.CountFingerprints(), reference.size());
                for (const uint64_t key : live)
                    REQUIRE(t.Contains(key));
            }
        }
        REQUIRE_EQ(t.CountFingerprints(), reference.size());
    }

    /**
     * Expansion deepens the buckets, and the expansion that would take the
     * depth to twice its base doubles the buckets instead and sheds a
     * fingerprint bit. Contraction is the exact inverse of both.
     */
    static void ExpansionRoundTrips(uint32_t r) {
        CuckooTable t(256, 24, hashmode::Default, 5, r);
        REQUIRE_EQ(t.GetBucketDepth(), CuckooTable::default_base_depth);
        std::mt19937_64 rng(r);
        std::vector<uint64_t> in;
        for (int32_t i = 0; i < 180; i++) {
            const uint64_t key = rng();
            if (t.Insert(key) == 0)
                in.push_back(key);
        }
        const uint64_t full_length = t.GetNumFingerprintBits();
        const uint64_t original_slots = t.CountSlots();
        const uint64_t entries = t.CountFingerprints();

        for (uint32_t e = 1; e <= 2 * r; e++) {
            const uint64_t predicted = t.CountSlotsAfterExpansion();
            REQUIRE_EQ(t.Expand(), entries);
            CheckStructure(t);
            REQUIRE_EQ(predicted, t.CountSlots());
            REQUIRE_EQ(t.GetExpansionCount(), e);
            // A bit goes per *period*, not per expansion.
            REQUIRE_EQ(t.GetNumFingerprintBits(), full_length - t.GetPeriodCount());
            REQUIRE_EQ(t.CountFingerprints(), entries);
            for (const uint64_t key : in)
                REQUIRE(t.Contains(key));
        }
        REQUIRE_EQ(t.GetPeriodCount(), 2);          // 2r expansions is two periods.
        REQUIRE_EQ(t.CountSlots(), original_slots * 4);

        while (t.GetExpansionCount() > 0) {
            REQUIRE_EQ(t.Contract(), entries);
            CheckStructure(t);
            for (const uint64_t key : in)
                REQUIRE(t.Contains(key));
        }
        REQUIRE_EQ(t.CountSlots(), original_slots);
        REQUIRE_EQ(t.GetNumFingerprintBits(), full_length);
        REQUIRE_EQ(t.CountFingerprints(), entries);
    }

    /** Counters follow their entries through the kicks that relocate them. */
    static void CountsFollowTheirEntries() {
        CuckooTable t(512, 26, hashmode::Default, 17);
        t.EnableCounters();
        std::mt19937_64 rng(18);
        std::map<uint64_t, uint64_t> counts;        // key -> the count we gave it
        std::vector<uint64_t> in;

        while (t.LoadFactor() < 0.9) {
            const uint64_t key = rng();
            const int64_t at = t.InsertAt(key);
            if (at < 0)
                break;
            const uint64_t count = 1 + (rng() % 1000);
            t.GetCounters()->Set(at, count);
            counts[key] = count;
            in.push_back(key);
        }
        CheckStructure(t);
        // Every key still reads back the count it was given, however many
        // kicks moved it since.
        for (const uint64_t key : in) {
            const int64_t at = t.FindMatch(key);
            REQUIRE_GE(at, 0);
            REQUIRE_EQ(t.GetCounters()->Get(at), counts[key]);
        }
    }

    /** The table refuses to grow once the fingerprint is down to a bit. */
    static void GrowthStopsWhenFingerprintsRunOut() {
        CuckooTable t(64, 12, hashmode::Default, 3);     // 16 buckets -> 8-bit fingerprints
        REQUIRE_EQ(t.GetNumFingerprintBits(), 8);
        std::mt19937_64 rng(4);
        for (int32_t i = 0; i < 30; i++)
            t.Insert(rng());
        const uint64_t entries = t.CountFingerprints();

        while (t.GetNumFingerprintBits() > 1) {
            REQUIRE_GT(t.CountSlotsAfterExpansion(), t.CountSlots());
            REQUIRE_EQ(t.Expand(), entries);
        }
        REQUIRE_EQ(t.GetNumFingerprintBits(), 1);
        const uint64_t slots = t.CountSlots();
        REQUIRE_EQ(t.CountSlotsAfterExpansion(), slots);
        REQUIRE_EQ(t.Expand(), CuckooTable::err_no_space);
        REQUIRE_EQ(t.CountSlots(), slots);           // Refused, and untouched.
    }

    /** Copy, move and reset. */
    static void CopyMoveAndReset() {
        CuckooTable t(512, 26, hashmode::Default, 21);
        std::mt19937_64 rng(22);
        std::vector<uint64_t> in;
        for (int32_t i = 0; i < 300; i++) {
            const uint64_t key = rng();
            if (t.Insert(key) == 0)
                in.push_back(key);
        }
        CuckooTable copy(t);
        CheckStructure(copy);
        REQUIRE_EQ(copy.CountFingerprints(), t.CountFingerprints());
        for (const uint64_t key : in)
            REQUIRE(copy.Contains(key));

        CuckooTable moved(std::move(copy));
        CheckStructure(moved);
        for (const uint64_t key : in)
            REQUIRE(moved.Contains(key));

        t.Reset();
        REQUIRE_EQ(t.CountFingerprints(), 0);
        for (const uint64_t key : in)
            REQUIRE_FALSE(t.Contains(key));
        CheckStructure(t);
    }
};

}   // namespace sublime

using sublime::CuckooTable;
using sublime::CuckooTableTest;

TEST_SUITE("CuckooTable") {
    TEST_CASE("fills to its load factor") {
        CuckooTableTest::FillsToItsLoadFactor(64, 24, 1);
        CuckooTableTest::FillsToItsLoadFactor(1024, 24, 2);
        CuckooTableTest::FillsToItsLoadFactor(4096, 30, 3);
    }

    TEST_CASE("insert and delete monte carlo") {
        CuckooTableTest::InsertDeleteMonteCarlo(512, 11);
        CuckooTableTest::InsertDeleteMonteCarlo(4096, 12);
    }

    TEST_CASE("expansion round trips") {
        CuckooTableTest::ExpansionRoundTrips(1);
        CuckooTableTest::ExpansionRoundTrips(2);
        CuckooTableTest::ExpansionRoundTrips(4);
    }

    TEST_CASE("counts follow their entries") {
        CuckooTableTest::CountsFollowTheirEntries();
    }

    TEST_CASE("growth stops when fingerprints run out") {
        CuckooTableTest::GrowthStopsWhenFingerprintsRunOut();
    }

    TEST_CASE("copy, move, and reset") {
        CuckooTableTest::CopyMoveAndReset();
    }
}
