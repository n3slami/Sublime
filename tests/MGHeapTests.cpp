#include <algorithm>
#include <map>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>

#include "CuckooTable.hpp"
#include "MG.hpp"
#include "MGHeap.hpp"

namespace sublime {

/**
 * White-box tests for `MGHeap`, a `friend` so they can reach the heap and the
 * offsets linking it to the table.
 *
 * The oracle is `MG` itself. The two run the same algorithm over the same
 * table, so they hold the same keys at the same counts -- but not in the same
 * slots, since the emptied entries `MGHeap` drains a couple at a time are slots
 * its table cannot use meanwhile. So their kick paths differ, and `MG` is an
 * exact oracle only until either table first gives up on one, as `MGTest`'s
 * reference Misra-Gries is for `MG`.
 */
class MGHeapTest {
public:
    using hashmode = MGHeap<>::hashmode;

    /**
     * Every invariant the heap rests on:
     *
     *  - it has one node per stored entry;
     *  - the two offsets are inverse: node `i` names a slot holding an entry,
     *    and that slot names `i` back, so the links are a bijection;
     *  - no stored count is below `L`, i.e. no count is negative (an emptied
 *    entry the drain has yet to reach sits at exactly `L`);
     *  - the heap is ordered.
     */
    static void CheckHeap(const MGHeap<>& h) {
        const auto& table = h.table_;
        REQUIRE_EQ(h.heap_size_, table.CountFingerprints());
        REQUIRE_LE(h.heap_size_, h.Capacity());

        std::unordered_set<uint64_t> occupied;
        for (auto it = table.begin(); it != table.end(); ++it) {
            occupied.insert(it.slot());
            const uint64_t pos = h.sidecar_.positions.Get(it.slot());
            REQUIRE_LT(pos, h.heap_size_);
            REQUIRE_EQ(h.slots_.Get(pos), it.slot());
        }
        for (uint64_t i = 0; i < h.heap_size_; i++) {
            const uint64_t slot = h.slots_.Get(i);
            REQUIRE(occupied.count(slot) == 1);
            REQUIRE_EQ(h.sidecar_.positions.Get(slot), i);
            REQUIRE_GE(h.counts_[i], h.lazy_decrement_);
            if (i > 0)
                REQUIRE_LE(h.counts_[(i - 1) / 2], h.counts_[i]);
        }
    }

    /** The links are exactly as wide as the indices they hold need. */
    static void CheckWidths(const MGHeap<>& h) {
        const auto ceil_log2 = [](uint64_t n) {
            uint32_t b = 1;
            while ((1ULL << b) < n)
                b++;
            return b;
        };
        REQUIRE_EQ(h.slots_.Width(), ceil_log2(h.table_.GetSlotCapacity()));
        REQUIRE_EQ(h.sidecar_.positions.Width(), ceil_log2(h.Capacity()));
    }

    struct Outcome {
        uint64_t lost = 0;
        uint64_t refused = 0;
        uint64_t lockstep_checks = 0;
    };

    /**
     * Replays a skewed random stream through `MGHeap` and `MG` side by side.
     *
     * Until either table first gives up on a kick path, the two must agree
     * exactly: the same live entries, the same decrements, the same answer for
     * every key in the universe. After that only what no loss can break is
     * checked: an intact heap, and no estimate above the true frequency of the
     * keys sharing its entry. The heap is checked every `check_at` insertions
     * throughout.
     */
    static Outcome AgreesWithMG(uint64_t nslots, uint32_t fingerprint_length, uint32_t seed,
                                uint64_t universe_size, uint64_t stream_length, bool prehashed,
                                uint64_t check_at = 500, uint64_t min_lockstep_checks = 8) {
        const hashmode mode = prehashed ? hashmode::Invertible : hashmode::Default;
        const uint8_t flags = prehashed ? MGHeap<>::flag_key_is_hash : 0;
        MGHeap<> heap(nslots, mode, seed, fingerprint_length);
        MG<> mg(nslots, mode, seed, fingerprint_length);
        REQUIRE_EQ(heap.Capacity(), mg.Capacity());
        CheckWidths(heap);

        std::mt19937_64 rng(seed * 2654435761ULL + 1);
        const auto draw = [&]() {
            uint64_t m = rng() % universe_size;         // Min-of-k: a cheap skew.
            for (int i = 0; i < 4; i++)
                m = std::min(m, rng() % universe_size);
            return m;
        };

        Outcome out;
        std::map<uint64_t, uint64_t> exact;             // Identity -> true frequency.
        bool lockstep = true;
        const auto check = [&]() {
            CheckHeap(heap);
            if (lockstep) {
                REQUIRE_EQ(heap.CountLive(), mg.CountMonitored());
                REQUIRE_EQ(heap.CountDecrements(), mg.CountDecrements());
                for (uint64_t key = 0; key < universe_size; key++)
                    REQUIRE_EQ(heap.Query(key, flags), mg.Query(key, flags));
                out.lockstep_checks++;
            }
            for (uint64_t key = 0; key < universe_size; key++) {
                const auto at = exact.find(heap.GetTable().EntryIdentity(key, flags));
                REQUIRE_LE(heap.Query(key, flags), at == exact.end() ? 0 : at->second);
            }
        };
        for (uint64_t t = 0; t < stream_length; t++) {
            const uint64_t key = draw();
            const int32_t status = heap.Insert(key, flags);
            const int32_t mg_status = lockstep ? mg.Insert(key, flags) : 0;
            exact[heap.GetTable().EntryIdentity(key, flags)]++;
            out.refused += (status != 0);
            lockstep = lockstep && status == 0 && mg_status == 0
                    && heap.GetTable().CountLostEntries() == 0 && mg.GetTable().CountLostEntries() == 0;
            if (t % check_at == 0)
                check();
        }
        check();
        REQUIRE_GT(heap.CountDecrements(), 0);          // The run reached case 3.
        REQUIRE_GE(out.lockstep_checks, min_lockstep_checks);   // The window was not vacuous.
        out.lost = heap.GetTable().CountLostEntries();
        return out;
    }
};


TEST_CASE("agrees with MG exactly") {
    MGHeapTest::AgreesWithMG(/*nslots=*/256, /*fp=*/10, /*seed=*/1, /*universe=*/8000, /*stream=*/300000, false);
    MGHeapTest::AgreesWithMG(1024, 10, 2, 50000, 800000, false);
    MGHeapTest::AgreesWithMG(512, 12, 43, 9000, 400000, false);
    MGHeapTest::AgreesWithMG(4096, 32, 44, 200000, 1000000, false, 5000);
}

TEST_CASE("collisions from short fingerprints") {
    MGHeapTest::AgreesWithMG(/*nslots=*/300, /*fp=*/5, /*seed=*/3, /*universe=*/4000, /*stream=*/300000, false);
    MGHeapTest::AgreesWithMG(512, 6, 4, 6000, 400000, false);
}

TEST_CASE("non-power-of-two sizes") {
    MGHeapTest::AgreesWithMG(/*nslots=*/777, /*fp=*/8, /*seed=*/5, /*universe=*/10000, /*stream=*/400000, false);
    MGHeapTest::AgreesWithMG(1000, 12, 6, 60000, 600000, false);
}

TEST_CASE("pre-hashed keys") {
    MGHeapTest::AgreesWithMG(/*nslots=*/512, /*fp=*/10, /*seed=*/7, /*universe=*/9000, /*stream=*/400000, true);
}

TEST_CASE("kick paths that lose a tenant or refuse the arrival") {
    // Tables of a few buckets give up on kick paths constantly, which is what
    // exercises the two ways an admission can come back damaged: a lost
    // tenant whose node must leave the heap, and a refused arrival whose node
    // must be popped. Checked after every insertion.
    MGHeapTest::Outcome total;
    for (uint32_t seed = 8; seed < 16; seed++) {
        const auto out = MGHeapTest::AgreesWithMG(/*nslots=*/16, /*fp=*/8, seed, /*universe=*/500,
                                                  /*stream=*/20000, false, /*check_at=*/1,
                                                  /*min_lockstep_checks=*/1);
        total.lost += out.lost;
        total.refused += out.refused;
    }
    const auto out = MGHeapTest::AgreesWithMG(8, 6, 9, 200, 20000, false, 1, 1);
    total.lost += out.lost;
    total.refused += out.refused;
    REQUIRE_GT(total.lost, 0);                          // Both paths were taken.
    REQUIRE_GT(total.refused, 0);
}

TEST_CASE("a decrement's emptied entries are drained, a couple per insertion") {
    // Everything monitored sits at exactly one, so the first key that finds the
    // summary full empties every one of them. It takes one of their slots, and
    // the rest leave at most `drain_batch` an insertion -- but all of them leave.
    MGHeap<> mg(256, MGHeap<>::hashmode::Invertible, 13, 20);
    const uint64_t key_is_hash = MGHeap<>::flag_key_is_hash;
    uint64_t admitted = 0;
    for (uint64_t k = 0; admitted < mg.Capacity(); k++) {
        if (mg.IsMonitored(k, key_is_hash))
            continue;               // Sharing an entry would leave it above one.
        mg.Insert(k, key_is_hash);
        admitted = mg.CountMonitored();
    }
    const uint64_t full = mg.CountMonitored();
    REQUIRE_EQ(full, mg.Capacity());

    mg.Insert(~0ULL, key_is_hash);
    REQUIRE_EQ(mg.CountDecrements(), 1);
    REQUIRE_EQ(mg.CountLive(), 1);                   // All emptied, one admitted...
    REQUIRE_EQ(mg.CountMonitored(), full - MGHeap<>::drain_batch);   // ...a couple drained.
    REQUIRE_EQ(mg.Query(~0ULL, key_is_hash), 1);
    for (uint64_t k = 0; k < 64; k++)
        REQUIRE_EQ(mg.Query(k, key_is_hash), 0);     // An emptied entry answers zero.
    MGHeapTest::CheckHeap(mg);

    uint64_t stored = mg.CountMonitored();
    while (stored > 1) {
        mg.Insert(~0ULL, key_is_hash);
        const uint64_t now = mg.CountMonitored();
        REQUIRE_EQ(stored - now, std::min<uint64_t>(MGHeap<>::drain_batch, stored - 1));
        stored = now;
        MGHeapTest::CheckHeap(mg);
    }
    REQUIRE_EQ(mg.CountLive(), 1);
    REQUIRE_EQ(mg.CountDecrements(), 1);
    REQUIRE_EQ(mg.Query(~0ULL, key_is_hash), 1 + (full - MGHeap<>::drain_batch - 1 + 1) / 2);
}

TEST_CASE("the slots drained after a decrement admit new keys without another") {
    // The emptied entries a decrement leaves are room: as the drain clears
    // them, the next arrivals are admitted into it, and `L` stays where it is.
    MGHeap<> mg(256, MGHeap<>::hashmode::Invertible, 19, 20);
    const uint64_t key_is_hash = MGHeap<>::flag_key_is_hash;
    uint64_t next = 0;
    while (mg.CountMonitored() < mg.Capacity()) {
        if (!mg.IsMonitored(next, key_is_hash))
            mg.Insert(next, key_is_hash);
        next++;
    }
    mg.Insert(~0ULL, key_is_hash);                   // The one decrement.
    REQUIRE_EQ(mg.CountDecrements(), 1);
    for (int i = 0; i < 20; i++) {
        while (mg.IsMonitored(next, key_is_hash))
            next++;
        REQUIRE_EQ(mg.Insert(next, key_is_hash), 0);
        REQUIRE_EQ(mg.Query(next, key_is_hash), 1);
        REQUIRE_EQ(mg.CountDecrements(), 1);
        MGHeapTest::CheckHeap(mg);
    }
}

TEST_CASE("an occurrence that frees nothing is dropped") {
    MGHeap<> mg(64, MGHeap<>::hashmode::Invertible, 17, 20);
    uint64_t admitted = 0;
    for (uint64_t k = 0; admitted < mg.Capacity(); k++) {
        if (mg.IsMonitored(k, MGHeap<>::flag_key_is_hash))
            continue;               // One key per entry, so every count is three.
        for (int rep = 0; rep < 3; rep++)
            mg.Insert(k, MGHeap<>::flag_key_is_hash);
        admitted = mg.CountMonitored();
    }
    const uint64_t full = mg.CountMonitored();

    mg.Insert(~0ULL, MGHeap<>::flag_key_is_hash);
    REQUIRE_EQ(mg.CountMonitored(), full);           // Nothing emptied...
    REQUIRE_EQ(mg.Query(~0ULL, MGHeap<>::flag_key_is_hash), 0);   // ...so nothing admitted.
    REQUIRE_EQ(mg.GetLazyDecrement(), 1);
    MGHeapTest::CheckHeap(mg);
}

TEST_CASE("reset empties and reuses") {
    MGHeap<> heap(512, MGHeap<>::hashmode::Default, 11, 8);
    for (uint64_t k = 0; k < 100000; k++)
        heap.Insert(k % 3000);
    REQUIRE_GT(heap.CountMonitored(), 0);
    heap.Reset();
    REQUIRE_EQ(heap.CountMonitored(), 0);
    REQUIRE_EQ(heap.CountDecrements(), 0);
    REQUIRE_EQ(heap.GetStreamLength(), 0);
    REQUIRE_EQ(heap.Query(5), 0);
    MGHeapTest::CheckHeap(heap);

    // A fresh MG sees the same stream as the reset heap does after it. The
    // table's kick randomness is not reset, so the two are compared only while
    // neither table has given up on a kick path.
    MG<> mg(512, MG<>::hashmode::Default, 11, 8);
    std::mt19937_64 rng(123);
    bool kicks_gave_up = false;
    for (uint64_t t = 0; t < 20000; t++) {
        const uint64_t key = std::min(rng() % 2000, rng() % 2000);
        kicks_gave_up |= heap.Insert(key) != 0;
        kicks_gave_up |= mg.Insert(key) != 0;
    }
    MGHeapTest::CheckHeap(heap);
    kicks_gave_up |= heap.GetTable().CountLostEntries() > 0 || mg.GetTable().CountLostEntries() > 0;
    if (!kicks_gave_up)
        for (uint64_t key = 0; key < 2000; key++)
            REQUIRE_EQ(heap.Query(key), mg.Query(key));
}

TEST_CASE("size counts the heap and both offsets") {
    MGHeap<> heap(1 << 12, MGHeap<>::hashmode::Default, 5, 32);
    MG<> mg(1 << 12, MG<>::hashmode::Default, 5, 32);
    // The same table and the same 32-bit counts, plus the two offset arrays:
    // log2(4096) = 12 bits a heap node, log2(capacity) = 12 bits a slot.
    const uint64_t capacity = heap.Capacity();
    const uint64_t slots = mg.GetTable().GetSlotCapacity();
    REQUIRE_EQ(slots, 1 << 12);
    const uint64_t expected = mg.GetTable().SizeInBytes() + capacity * sizeof(uint32_t)
                            + (capacity * 12 + 7) / 8 + (slots * 12 + 7) / 8;
    REQUIRE_EQ(heap.SizeInBytes(), expected);
    MGHeapTest::CheckWidths(heap);
}

}   // namespace sublime
