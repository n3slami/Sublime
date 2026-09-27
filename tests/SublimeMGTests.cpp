#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>

#include "MG.hpp"
#include "CuckooTable.hpp"
#include "SublimeMG.hpp"

namespace sublime {

/**
 * White-box tests for `SublimeMG`. Declared a `friend` of both halves, so
 * these can check that the counters really do line up with the slots.
 */
class SublimeMGTest {
public:
    using hashmode = SublimeMG<>::hashmode;

    /**
     * The invariant the whole design rests on: counter `i` belongs to the
     * fingerprint in slot `i`. Checks that every slot the table considers
     * occupied carries the count `ref` says it should, and -- just as
     * important -- that every slot it considers empty carries nothing.
     */
    template <typename MGT>
    static void CheckMirrored(const MGT& mg,
                              const std::map<std::pair<uint64_t, uint64_t>, uint64_t>& ref) {
        const auto& table = mg.table_;
        const VALECounters& counters = mg.Counters();
        REQUIRE_EQ(counters.CountCounters(), table.GetSlotCapacity());

        uint64_t seen = 0;
        std::vector<bool> occupied(table.GetSlotCapacity(), false);
        for (auto it = table.begin(); it != table.end(); ++it) {
            const uint64_t slot = it.slot();
            occupied[slot] = true;
            const auto entry = ref.find({it.bucket(), it.fingerprint()});
            REQUIRE(entry != ref.end());
            REQUIRE_EQ(counters.Get(slot), entry->second);
            seen++;
        }
        REQUIRE_EQ(seen, ref.size());
        REQUIRE_EQ(seen, table.CountFingerprints());

        // No count is left stranded on a slot that holds no fingerprint.
        for (uint64_t i = 0; i < table.GetSlotCapacity(); i++)
            if (!occupied[i])
                REQUIRE_EQ(counters.Get(i), 0);
    }

    /** The reference key for the entry a key maps to. */
    template <typename MGT>
    static std::pair<uint64_t, uint64_t> RefKey(const MGT& mg, uint64_t key) {
        const uint64_t hash = mg.table_.hash_key(key, 0);
        return {mg.table_.bucket_from_hash(hash), mg.table_.fingerprint_from_hash(hash)};
    }

    /**
     * @returns `count` keys that all land on distinct entries, so that a
     * reference model keyed by the entry is exact rather than approximate.
     */
    template <typename MGT>
    static std::vector<uint64_t> DistinctKeys(const MGT& mg, uint64_t count, uint64_t seed) {
        std::mt19937_64 rng(seed);
        std::set<std::pair<uint64_t, uint64_t>> used;
        std::vector<uint64_t> res;
        while (res.size() < count) {
            const uint64_t key = rng();
            if (used.insert(RefKey(mg, key)).second)
                res.push_back(key);
        }
        return res;
    }

    /**
     * The weaker invariant that survives a resize, when the entry a key maps
     * to changes: every slot the table calls occupied carries a count of at
     * least one -- an entry a decrement empties is evicted -- and every slot
     * it calls empty carries nothing at all.
     */
    template <typename MGT>
    static void CheckNoCountIsStranded(const MGT& mg) {
        const auto& table = mg.table_;
        const VALECounters& counters = mg.Counters();
        REQUIRE_EQ(counters.CountCounters(), table.GetSlotCapacity());

        std::vector<bool> occupied(table.GetSlotCapacity(), false);
        uint64_t seen = 0;
        for (auto it = table.begin(); it != table.end(); ++it) {
            occupied[it.slot()] = true;
            REQUIRE_GE(counters.Get(it.slot()), 1);
            seen++;
        }
        REQUIRE_EQ(seen, table.CountFingerprints());
        for (uint64_t i = 0; i < table.GetSlotCapacity(); i++)
            if (!occupied[i])
                REQUIRE_EQ(counters.Get(i), 0);
    }

    /** @returns Every count stored in the table, in slot order. */
    template <typename MGT>
    static std::vector<uint64_t> StoredCounts(const MGT& mg) {
        std::vector<uint64_t> res;
        for (auto it = mg.table_.begin(); it != mg.table_.end(); ++it)
            res.push_back(mg.Counters().Get(it.slot()));
        return res;
    }

    /**
     * What has to hold of the summary between operations, on top of the
     * mirroring: every entry's count stands strictly above zero -- an entry a
     * decrement emptied is evicted, never left behind -- one entry is
     * monitored per stored fingerprint, and no more keys are monitored than
     * the summary has room for.
     */
    template <typename MGT>
    static void CheckSummary(const MGT& mg) {
        CheckNoCountIsStranded(mg);
        uint64_t entries = 0, smallest = 0;
        for (auto it = mg.table_.begin(); it != mg.table_.end(); ++it) {
            const uint64_t stored = mg.Counters().Get(it.slot());
            REQUIRE_GT(stored, 0);
            // Nothing may sit below the decrement it owes: an entry at exactly
            // the decrement is a key whose count has reached zero, waiting to
            // be evicted, and one below it would read as a negative count.
            REQUIRE_GE(stored, mg.GetLazyDecrement());
            if (smallest == 0 || stored < smallest)
                smallest = stored;
            entries++;
        }
        REQUIRE_EQ(entries, mg.CountMonitored());
        REQUIRE_LE(mg.CountMonitored(), mg.Capacity());

        if (mg.Counters().HasMinTree()) {
            // The tree's root is that smallest counter, and its candidate is
            // an entry holding it -- worked out here by walking the table,
            // independently of the tree's own bookkeeping.
            REQUIRE_EQ(mg.Counters().MinValue(), smallest);
            if (smallest != 0)
                REQUIRE_EQ(mg.Counters().Get(mg.Counters().MinSlot()), smallest);
        }
        else {
            REQUIRE_EQ(mg.GetLazyDecrement(), 0);
        }
    }

    /** @returns The exact frequency of every key of a stream. */
    static std::map<uint64_t, uint64_t> Frequencies(const std::vector<uint64_t>& stream) {
        std::map<uint64_t, uint64_t> res;
        for (const uint64_t key : stream)
            res[key]++;
        return res;
    }

    /**
     * Checks the two halves of the Misra-Gries guarantee against an exact
     * count of the stream: no key is ever over-counted, no key is under-counted
     * by more than the number of decrements the summary performed, and every
     * key frequent enough to clear that many decrements is still monitored.
     *
     * @param capacity_bound How many counts a single decrement took one off,
     * which bounds how many decrements the stream could pay for. Defaults to
     * the summary's capacity, and has to be given explicitly when the summary
     * grew during the run: the decrements happened while it was smaller, so
     * the capacity it ended at is no bound on them.
     * @returns How many keys of the stream were frequent enough for the
     * guarantee to say anything about them.
     */
    /**
     * How many entries the monitored set has dropped on its own -- which only
     * a cuckoo filter does, when a kick path runs out of patience. The
     * quotient filter never loses one, so it always answers 0.
     */
    template <typename MGT>
    static uint64_t LostEntries(const MGT& mg) {
        if constexpr (std::is_same_v<typename MGT::table_type, sublime::CuckooTable>)
            return mg.GetTable().CountLostEntries();
        else
            return 0;
    }

    template <typename MGT>
    static uint64_t CheckMisraGriesGuarantee(const MGT& mg,
                                             const std::vector<uint64_t>& stream,
                                             uint64_t capacity_bound = 0) {
        const auto truth = Frequencies(stream);
        const uint64_t decrements = mg.CountDecrements();
        // An entry the table dropped takes its whole count with it, and an
        // occurrence the table had no room for is never counted at all, so
        // neither side of the understatement bound survives a loss. The
        // over-estimation side does: a count only ever rises on an occurrence
        // of a key that matches it.
        const bool lossy = LostEntries(mg) > 0;
        uint64_t heavy = 0;
        for (const auto& [key, frequency] : truth) {
            const uint64_t estimate = mg.Query(key);
            REQUIRE_LE(estimate, frequency);
            if (lossy) {
                heavy += (frequency > decrements && estimate > 0);
                continue;
            }
            REQUIRE_GE(estimate + decrements, frequency);
            if (frequency > decrements) {
                REQUIRE_GT(estimate, 0);
                heavy++;
            }
        }
        // Each decrement takes one off every monitored count, so the stream
        // has to have been long enough to have put that much in.
        REQUIRE_LE(decrements * (capacity_bound ? capacity_bound : mg.Capacity()), stream.size());
        return heavy;
    }

    static void InsertIncrementsTheMatch() {
        SublimeMG<> mg(1024, 26, hashmode::Default, 1);
        REQUIRE_EQ(mg.CountMonitored(), 0);

        REQUIRE_FALSE(mg.IsMonitored(42));
        REQUIRE_EQ(mg.Query(42), 0);

        // Monitoring it starts it at one, and inserting counts up from there.
        REQUIRE_EQ(mg.StartMonitoring(42), 0);
        REQUIRE(mg.IsMonitored(42));
        REQUIRE_EQ(mg.Query(42), 1);
        REQUIRE_EQ(mg.CountMonitored(), 1);
        for (uint64_t i = 2; i <= 100; i++) {
            REQUIRE_EQ(mg.Insert(42), 0);
            REQUIRE_EQ(mg.Query(42), i);
        }
        // Counting past a stub, so the count grows an extension.
        for (uint64_t i = 101; i <= 50000; i++)
            REQUIRE_EQ(mg.Insert(42), 0);
        REQUIRE_EQ(mg.Query(42), 50000);
        REQUIRE_EQ(mg.CountMonitored(), 1);

        // Evicting it takes the count with it.
        REQUIRE_EQ(mg.StopMonitoring(42), 0);
        REQUIRE_FALSE(mg.IsMonitored(42));
        REQUIRE_EQ(mg.Query(42), 0);
        REQUIRE_EQ(mg.CountMonitored(), 0);
        // ... and a key monitored again afterwards starts from scratch.
        REQUIRE_EQ(mg.StartMonitoring(42), 0);
        REQUIRE_EQ(mg.Query(42), 1);
    }

    /**
     * The point of the exercise: as insertions push slots around inside the
     * table, every count has to travel with the fingerprint it belongs to.
     */
    static void CountsFollowTheirFingerprints() {
        SublimeMG<> mg(512, 26, hashmode::Default, 3);
        const auto keys = DistinctKeys(mg, 400, 11);
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> ref;

        // Monitor the keys one at a time, counting each up to a different,
        // deliberately awkward value: some stay inside a stub, some need an
        // extension, some are large enough to push a chunk onto a tails array.
        for (size_t i = 0; i < keys.size(); i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
            ref[RefKey(mg, keys[i])] = 1;

            const uint64_t target = (i % 5 == 0) ? 3
                                  : (i % 5 == 1) ? 40
                                  : (i % 5 == 2) ? 1000
                                  : (i % 5 == 3) ? 20000
                                                 : 90000;
            for (uint64_t k = 1; k < target; k++)
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
            ref[RefKey(mg, keys[i])] = target;

            // Every key monitored so far still reads back exactly, however
            // much the insertion just shifted the table around.
            if (i % 37 == 0 || i + 1 == keys.size())
                CheckMirrored(mg, ref);
        }
        CheckMirrored(mg, ref);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), ref[RefKey(mg, key)]);

        // Evicting keys shifts slots the other way; the survivors keep their
        // counts through it.
        for (size_t i = 0; i < keys.size(); i += 2) {
            REQUIRE_EQ(mg.StopMonitoring(keys[i]), 0);
            ref.erase(RefKey(mg, keys[i]));
            if (i % 40 == 0)
                CheckMirrored(mg, ref);
        }
        CheckMirrored(mg, ref);
        for (size_t i = 1; i < keys.size(); i += 2)
            REQUIRE_EQ(mg.Query(keys[i]), ref[RefKey(mg, keys[i])]);
    }

    /**
     * Keys forced into a single run, so that every insertion shifts a long
     * cluster and the counters are dragged across chunk boundaries.
     */
    static void CountsSurviveLongClusters() {
        // `None` hashing lets us aim keys at whatever bucket we like.
        SublimeMG<> mg(1024, 26, hashmode::None, 1);
        const uint64_t bihs = mg.table_.GetBucketIndexHashSize();
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> ref;

        // Every key here shares bucket 300 and differs only in its
        // fingerprint, so they all pile into one run.
        std::vector<uint64_t> keys;
        for (uint64_t i = 0; i < 300; i++) {
            const uint64_t hash = 300 | ((i * 2654435761ULL) << bihs);
            keys.push_back(hash);
        }
        for (size_t i = 0; i < keys.size(); i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i], SublimeMG<>::flag_key_is_hash), 0);
            ref[RefKey(mg, keys[i])] = 1;
            const uint64_t target = 1 + (i * 7919) % 40000;
            for (uint64_t k = 1; k < target; k++)
                REQUIRE_EQ(mg.Insert(keys[i], SublimeMG<>::flag_key_is_hash), 0);
            ref[RefKey(mg, keys[i])] = target;
        }
        CheckMirrored(mg, ref);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key, SublimeMG<>::flag_key_is_hash), ref[RefKey(mg, key)]);

        // Drain the run from the middle out, so the deletions shift long
        // stretches of counters both ways.
        while (!keys.empty()) {
            const size_t index = keys.size() / 2;
            REQUIRE_EQ(mg.StopMonitoring(keys[index], SublimeMG<>::flag_key_is_hash), 0);
            ref.erase(RefKey(mg, keys[index]));
            keys.erase(keys.begin() + index);
            if (keys.size() % 50 == 0)
                CheckMirrored(mg, ref);
        }
        CheckMirrored(mg, ref);
        REQUIRE_EQ(mg.CountMonitored(), 0);
    }

    /** Monitoring, counting, and evicting at random, checked throughout. */
    static void MonteCarlo() {
        SublimeMG<> mg(256, 26, hashmode::Default, 7);
        const auto keys = DistinctKeys(mg, 500, 13);
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> ref;
        std::vector<uint64_t> monitored;
        std::mt19937_64 rng(14);

        for (int32_t step = 0; step < 30000; step++) {
            const uint32_t action = rng() % 100;
            if (action < 10 || monitored.empty()) {
                // Admit a key that is not already monitored, so that the
                // reference stays keyed one-to-one by entry.
                const uint64_t key = keys[rng() % keys.size()];
                if (mg.IsMonitored(key))
                    continue;
                if (mg.CountMonitored() + 1 >= mg.GetTable().CountSlots() * FingerprintTable::max_load_factor)
                    continue;
                REQUIRE_EQ(mg.StartMonitoring(key), 0);
                ref[RefKey(mg, key)] = 1;
                monitored.push_back(key);
            }
            else if (action < 95) {
                const uint64_t key = monitored[rng() % monitored.size()];
                REQUIRE_EQ(mg.Insert(key), 0);
                ref[RefKey(mg, key)]++;
            }
            else {
                const size_t index = rng() % monitored.size();
                const uint64_t key = monitored[index];
                monitored[index] = monitored.back();
                monitored.pop_back();
                REQUIRE_EQ(mg.StopMonitoring(key), 0);
                ref.erase(RefKey(mg, key));
            }

            if (step % 500 == 0)
                CheckMirrored(mg, ref);
        }
        CheckMirrored(mg, ref);

        // The summary drains completely, counters and all.
        for (const uint64_t key : monitored)
            REQUIRE_EQ(mg.StopMonitoring(key), 0);
        ref.clear();
        CheckMirrored(mg, ref);
        REQUIRE_EQ(mg.CountMonitored(), 0);
    }

    /**
     * Counts large enough to spill chunks into tails arrays trigger a retune,
     * which re-lays-out the counters more tightly without losing any of them.
     */
    static void RetunesUnderLargeCounts() {
        SublimeMG<> mg(4000, 30, hashmode::Default, 5);
        const auto keys = DistinctKeys(mg, 3000, 15);
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> ref;
        for (const uint64_t key : keys) {
            REQUIRE_EQ(mg.StartMonitoring(key), 0);
            ref[RefKey(mg, key)] = 1;
        }
        const uint32_t stub_before = mg.Counters().GetStubLength();
        const uint32_t cpc_before = mg.Counters().GetCountersPerChunk();

        // Count every key up well past what a default five-bit stub holds.
        std::mt19937_64 rng(16);
        for (const uint64_t key : keys) {
            const uint64_t target = 20000 + rng() % 20000;
            for (uint64_t k = 1; k < target; k++)
                REQUIRE_EQ(mg.Insert(key), 0);
            ref[RefKey(mg, key)] = target;
        }

        // VALE noticed and re-tuned itself: longer stubs for longer counts.
        REQUIRE_GT(mg.Counters().GetStubLength(), stub_before);
        REQUIRE_LT(mg.Counters().GetCountersPerChunk(), cpc_before);
        REQUIRE_EQ(mg.Counters().CountChunksWithTails(), 0);
        CheckMirrored(mg, ref);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), ref[RefKey(mg, key)]);

        // And the summary keeps working across the retune.
        for (const uint64_t key : keys) {
            REQUIRE_EQ(mg.Insert(key), 0);
            ref[RefKey(mg, key)]++;
        }
        CheckMirrored(mg, ref);
    }

    /** Every count follows its key across an expansion. */
    static void ExpansionCarriesCounts() {
        for (const uint32_t r : {1u, 3u}) {
            SublimeMG<> mg(512, 30, hashmode::Default, r, r);
            const auto keys = DistinctKeys(mg, 300, 21);
            std::map<uint64_t, uint64_t> ref;
            for (size_t i = 0; i < keys.size(); i++) {
                REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
                // Counts spanning stubs, extensions, and tails arrays.
                const uint64_t target = 1 + (i * 977) % 30000;
                for (uint64_t k = 1; k < target; k++)
                    REQUIRE_EQ(mg.Insert(keys[i]), 0);
                ref[keys[i]] = target;
            }
            CheckNoCountIsStranded(mg);

            for (uint32_t e = 1; e <= 3 * r; e++) {
                REQUIRE_EQ(mg.Expand(), keys.size());   // One entry in, one entry out.
                CheckNoCountIsStranded(mg);
                REQUIRE_EQ(mg.CountMonitored(), keys.size());
                REQUIRE_EQ(mg.Counters().CountCounters(), mg.GetTable().GetSlotCapacity());
                for (const auto& [key, count] : ref)
                    REQUIRE_EQ(mg.Query(key), count);
            }

            // The summary still works afterwards, at its new size.
            for (const uint64_t key : keys) {
                REQUIRE_EQ(mg.Insert(key), 0);
                ref[key]++;
            }
            CheckNoCountIsStranded(mg);
            for (const auto& [key, count] : ref)
                REQUIRE_EQ(mg.Query(key), count);
        }
    }

    /** Contraction gives back exactly what expansion took, counts included. */
    static void ContractionRestoresCounts() {
        for (const uint32_t r : {1u, 2u}) {
            SublimeMG<> mg(512, 30, hashmode::Default, r + 4, r);
            const auto keys = DistinctKeys(mg, 200, 23);
            std::map<uint64_t, uint64_t> ref;
            for (size_t i = 0; i < keys.size(); i++) {
                REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
                const uint64_t target = 1 + (i * 613) % 50000;
                for (uint64_t k = 1; k < target; k++)
                    REQUIRE_EQ(mg.Insert(keys[i]), 0);
                ref[keys[i]] = target;
            }
            const auto counts_before = StoredCounts(mg);
            const uint64_t slots_before = mg.GetTable().CountSlots();

            const uint32_t steps = 2 * r + 1;
            for (uint32_t i = 0; i < steps; i++)
                REQUIRE_EQ(mg.Expand(), keys.size());
            for (uint32_t i = 0; i < steps; i++) {
                REQUIRE_EQ(mg.Contract(), keys.size());
                CheckNoCountIsStranded(mg);
                for (const auto& [key, count] : ref)
                    REQUIRE_EQ(mg.Query(key), count);
            }
            REQUIRE_EQ(mg.GetTable().CountSlots(), slots_before);
            // Right down to which slot holds which count.
            REQUIRE(StoredCounts(mg) == counts_before);
        }
    }

    /** Monitoring, counting, evicting, expanding, and contracting, interleaved. */
    static void ResizeMonteCarlo() {
        SublimeMG<> mg(256, 34, hashmode::Default, 29, 3);
        const auto keys = DistinctKeys(mg, 400, 25);
        std::map<uint64_t, uint64_t> ref;
        std::vector<uint64_t> monitored;
        std::mt19937_64 rng(26);

        for (int32_t step = 0; step < 6000; step++) {
            const uint32_t action = rng() % 100;
            if (action < 3 && mg.GetTable().GetNumKeyBits() < 44) {
                REQUIRE_GE(mg.Expand(), 0);
                CheckNoCountIsStranded(mg);
            }
            else if (action < 6 && mg.GetTable().GetExpansionCount() > 0
                     && mg.CountMonitored() < mg.GetTable().CountSlots() / 2
                                                * FingerprintTable::max_load_factor) {
                REQUIRE_GE(mg.Contract(), 0);
                CheckNoCountIsStranded(mg);
            }
            else if (action < 20 || monitored.empty()) {
                const uint64_t key = keys[rng() % keys.size()];
                if (mg.IsMonitored(key))
                    continue;
                if (mg.CountMonitored() + 1 >= mg.GetTable().CountSlots()
                                                * FingerprintTable::max_load_factor)
                    continue;
                REQUIRE_EQ(mg.StartMonitoring(key), 0);
                ref[key] = 1;
                monitored.push_back(key);
            }
            else if (action < 95) {
                const uint64_t key = monitored[rng() % monitored.size()];
                REQUIRE_EQ(mg.Insert(key), 0);
                ref[key]++;
            }
            else {
                const size_t index = rng() % monitored.size();
                const uint64_t key = monitored[index];
                monitored[index] = monitored.back();
                monitored.pop_back();
                REQUIRE_EQ(mg.StopMonitoring(key), 0);
                ref.erase(key);
            }

            // Every monitored key reads back its count. The fingerprints stay
            // long enough here that none of these keys collide, so the
            // over-estimate a Misra-Gries count is allowed to be is exact.
            for (const auto& [key, count] : ref)
                REQUIRE_EQ(mg.Query(key), count);
        }
        CheckNoCountIsStranded(mg);
    }

    static void CopyMoveAndReset() {
        SublimeMG<> mg(256, 24, hashmode::Default, 1);
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> ref;
        const auto keys = DistinctKeys(mg, 100, 17);
        for (size_t i = 0; i < keys.size(); i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
            for (uint64_t k = 0; k < i * 37; k++)
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
            ref[RefKey(mg, keys[i])] = 1 + i * 37;
        }
        CheckMirrored(mg, ref);

        SublimeMG<> copy(mg);
        CheckMirrored(copy, ref);
        // The copy owns counters of its own, so writing through one summary
        // must not show up in the other.
        REQUIRE_NE(copy.table_.GetCounters(), mg.table_.GetCounters());
        REQUIRE_EQ(copy.StopMonitoring(keys[0]), 0);
        REQUIRE_EQ(mg.Query(keys[0]), 1);
        CheckMirrored(mg, ref);

        SublimeMG<> moved(std::move(copy));
        REQUIRE_EQ(moved.Query(keys[1]), ref[RefKey(mg, keys[1])]);

        SublimeMG<> assigned(16, 12, hashmode::None, 2);
        assigned = mg;
        REQUIRE_NE(assigned.table_.GetCounters(), mg.table_.GetCounters());
        CheckMirrored(assigned, ref);

        mg.Reset();
        REQUIRE_EQ(mg.CountMonitored(), 0);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), 0);
        CheckMirrored(mg, {});
        CheckMirrored(assigned, ref);

        // It works again after the reset.
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.StartMonitoring(key), 0);
        REQUIRE_EQ(mg.CountMonitored(), keys.size());
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), 1);
    }

    /*
     * ------------------------------------------------------------------
     * The Misra-Gries insertion algorithm.
     * ------------------------------------------------------------------
     */

    /** While there is room, an unmonitored key is simply admitted at one. */
    static void AdmitsWhileThereIsRoom() {
        SublimeMG<> mg(128, 26, hashmode::Default, 1);
        const uint64_t capacity = mg.Capacity();
        REQUIRE_EQ(capacity, static_cast<uint64_t>(128 * FingerprintTable::max_load_factor));

        const auto keys = DistinctKeys(mg, capacity, 31);
        for (uint64_t i = 0; i < capacity; i++) {
            REQUIRE_EQ(mg.Insert(keys[i]), 0);
            REQUIRE_EQ(mg.CountMonitored(), i + 1);
            REQUIRE_EQ(mg.Query(keys[i]), 1);
            // Nothing is decremented while there is still room.
            REQUIRE_EQ(mg.CountDecrements(), 0);
        }
        CheckSummary(mg);
        // A second occurrence of a monitored key just counts up.
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Insert(key), 0);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), 2);
        CheckSummary(mg);
    }

    /**
     * The sweep evicts exactly the counters it empties -- the smallest ones,
     * in order -- and the arrival that paid for it takes one of the slots it
     * freed. An arrival that took a slot is then itself decremented by the
     * next sweep, exactly as Misra-Gries would have it.
     *
     * That last part is what makes the arithmetic here interesting. Against a
     * summary holding counts 1, 2, 3, ... the first arrival sweeps once and
     * takes the slot the count of one leaves. The next arrival finds the
     * summary full again and sweeps a second time, which empties the count of
     * two *and* the first arrival -- two slots, so the arrival after it walks
     * straight into the room that leaves without sweeping at all. The third
     * sweep frees three, and so on: the arrivals admitted between two sweeps
     * all die at the next one, so the sweeps come in a triangular pattern,
     * `1 + 2 + 3 + ...` arrivals apart, and only the arrivals after the last
     * one are still standing at the end.
     */
    static void EvictsTheSmallestCounters() {
        const uint64_t arrivals = 8;
        SublimeMG<> mg(128, 30, hashmode::Default, 1);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + arrivals, 33);

        // Counts 1, 2, 3, ... so that the order the evictions come in is known.
        for (uint64_t i = 0; i < capacity; i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
            for (uint64_t k = 1; k <= i; k++)
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
            REQUIRE_EQ(mg.Query(keys[i]), i + 1);
        }
        REQUIRE_EQ(mg.CountMonitored(), capacity);

        // Now keys the summary has never seen, one at a time.
        for (uint64_t i = 0; i < arrivals; i++)
            REQUIRE_EQ(mg.Insert(keys[capacity + i]), 0);
        CheckSummary(mg);

        // 1 + 2 + 3 = 6 of the eight arrivals fell on a sweep, the fourth of
        // which freed four slots with only two arrivals left to fill them.
        const uint64_t decrements = 4;
        const uint64_t survivors = 2;
        REQUIRE_EQ(mg.CountDecrements(), decrements);

        // The count-1 through count-`decrements` entries are the ones it
        // reached, and everything above them came down by exactly that many.
        for (uint64_t i = 0; i < decrements; i++)
            REQUIRE_FALSE(mg.IsMonitored(keys[i]));
        for (uint64_t i = decrements; i < capacity; i++)
            REQUIRE_EQ(mg.Query(keys[i]), i + 1 - decrements);

        // Only the keys that arrived after the last sweep are still there;
        // the earlier ones were admitted and decremented back out again.
        for (uint64_t i = 0; i + survivors < arrivals; i++)
            REQUIRE_FALSE(mg.IsMonitored(keys[capacity + i]));
        for (uint64_t i = arrivals - survivors; i < arrivals; i++)
            REQUIRE_EQ(mg.Query(keys[capacity + i]), 1);
        REQUIRE_EQ(mg.CountMonitored(), capacity - decrements + survivors);
    }

    /**
     * A summary whose counts are all one: a single sweep empties every one of
     * them at once, and every entry it emptied has to go -- nothing may be
     * left standing at zero.
     */
    static void ManyTiedCountersEmptyAtOnce() {
        const uint64_t arrivals = 8;
        SublimeMG<> mg(256, 30, hashmode::Default, 1);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + arrivals, 34);

        // Every single count is one, so the first sweep zeroes all of them.
        for (uint64_t i = 0; i < capacity; i++)
            REQUIRE_EQ(mg.Insert(keys[i]), 0);
        REQUIRE_EQ(mg.CountMonitored(), capacity);

        for (uint64_t i = 0; i < arrivals; i++)
            REQUIRE_EQ(mg.Insert(keys[capacity + i]), 0);
        CheckSummary(mg);

        // One sweep emptied the summary, and the arrivals walked into the room
        // it left: only the first of them had to pay for it.
        REQUIRE_EQ(mg.CountDecrements(), 1);
        for (uint64_t i = 0; i < capacity; i++)
            REQUIRE_FALSE(mg.IsMonitored(keys[i]));
        REQUIRE_EQ(mg.CountMonitored(), arrivals);
        for (uint64_t i = 0; i < arrivals; i++)
            REQUIRE_EQ(mg.Query(keys[capacity + i]), 1);
    }

    /** The guarantee itself, on a skewed stream. */
    static void KeepsTheMisraGriesGuarantee() {
        for (const uint32_t seed : {37u, 91u, 113u}) {
            SublimeMG<> mg(1024, 34, hashmode::Default, seed);
            std::mt19937_64 rng(seed + 1);
            std::vector<uint64_t> stream;
            for (int32_t i = 0; i < 300000; i++) {
                // Skewed: a few keys dominate, with a long tail behind them.
                const double u = (rng() % 1000000) / 1000000.0;
                const uint64_t key = static_cast<uint64_t>(50000 * u * u * u * u);
                stream.push_back(key);
                REQUIRE_EQ(mg.Insert(key), 0);
            }
            CheckSummary(mg);
            REQUIRE_GT(CheckMisraGriesGuarantee(mg, stream), 0);
        }
    }

    /**
     * At a fixed size, `SublimeMG` and `MG` are the same algorithm over the
     * same monitored set: same table, same fingerprint length, same seed, and
     * the only difference is that one keeps its counts in VALE's
     * variable-length counters and the other in a plain `uint32_t` array. So
     * every query has to agree exactly, insertion for insertion -- and the
     * space they take must not.
     */
    static void AgreesWithPlainMisraGries() {
        const uint64_t nslots = 512;
        const uint32_t fingerprint_length = 20, seed = 4242;
        uint64_t quotient_bits = 0;
        for (uint64_t n = nslots; n > 1; n >>= 1)
            quotient_bits++;

        SublimeMG<> sublime(nslots, quotient_bits + fingerprint_length,
                            hashmode::Default, seed);
        MG plain(nslots, hashmode::Default, seed, fingerprint_length);
        REQUIRE_EQ(sublime.Capacity(), plain.Capacity());

        std::mt19937_64 rng(4243);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 200000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(30000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(sublime.Insert(key), 0);
            plain.Insert(key);
            if (i % 20000 == 0) {
                REQUIRE_EQ(sublime.CountMonitored(), plain.CountMonitored());
                REQUIRE_EQ(sublime.CountDecrements(), plain.CountDecrements());
            }
        }
        CheckSummary(sublime);
        REQUIRE_EQ(sublime.CountMonitored(), plain.CountMonitored());
        REQUIRE_EQ(sublime.CountDecrements(), plain.CountDecrements());
        REQUIRE_GT(sublime.CountDecrements(), 0);

        for (const auto& [key, frequency] : Frequencies(stream))
            REQUIRE_EQ(sublime.Query(key), plain.Query(key));

        // Which is the whole point: the same answers, in less space.
        REQUIRE_LT(sublime.SizeInBytes(), plain.SizeInBytes());
    }

    /*
     * ------------------------------------------------------------------
     * The min segment tree.
     * ------------------------------------------------------------------
     */

    /** A skewed stream through one instantiation or another, checked throughout. */
    template <typename MGT = SublimeMG<true, true>>
    static void TreeMonteCarlo(uint64_t nslots, uint64_t universe, int32_t steps, uint32_t seed) {
        MGT mg(nslots, 34, hashmode::Default, seed);
        std::mt19937_64 rng(seed + 1);
        std::vector<uint64_t> stream;
        const int32_t check_at = steps / 20 + 1;
        for (int32_t i = 0; i < steps; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(universe * u * u * u);
            stream.push_back(key);
            // A cuckoo filter can turn an insertion away with room left in the
            // table, which `SublimeMG` reports and the stream goes on past.
            const int32_t status = mg.Insert(key);
            REQUIRE((status == 0 || status == MGT::err_no_space));
            if (i % check_at == 0)
                CheckSummary(mg);
        }
        CheckSummary(mg);
        REQUIRE_GT(CheckMisraGriesGuarantee(mg, stream), 0);
    }

    /**
     * The keys a decrement empties all go at once, and the decrement is paid
     * for once. Admitting every key at the same moment leaves them all at the
     * same count, so the first arrival that finds the summary full takes the
     * whole table with it.
     */
    static void TreeEvictsEveryEmptiedKeyAtOnce() {
        SublimeMG<true, true> mg(128, 30, hashmode::Default, 5);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + 4, 6);
        for (uint64_t i = 0; i < capacity; i++)
            REQUIRE_EQ(mg.Insert(keys[i]), 0);
        REQUIRE_EQ(mg.CountMonitored(), capacity);
        REQUIRE_EQ(mg.GetLazyDecrement(), 0);
        CheckSummary(mg);

        // Every count is one, so one decrement empties all of them.
        REQUIRE_EQ(mg.Insert(keys[capacity]), 0);
        REQUIRE_EQ(mg.CountDecrements(), 1);
        REQUIRE_EQ(mg.GetLazyDecrement(), 1);
        REQUIRE_EQ(mg.CountMonitored(), 1);             // All gone, the arrival admitted.
        REQUIRE_EQ(mg.Query(keys[capacity]), 1);
        for (uint64_t i = 0; i < capacity; i++)
            REQUIRE_EQ(mg.Query(keys[i]), 0);
        CheckSummary(mg);

        // And the arrivals behind it walk into the room that left, without
        // paying for a decrement of their own.
        REQUIRE_EQ(mg.Insert(keys[capacity + 1]), 0);
        REQUIRE_EQ(mg.CountDecrements(), 1);
        REQUIRE_EQ(mg.CountMonitored(), 2);
        CheckSummary(mg);
    }

    /** An arrival that empties nothing is dropped, and pays for its decrement. */
    static void TreeDropsAnArrivalThatFreesNothing() {
        SublimeMG<true, true> mg(128, 30, hashmode::Default, 7);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + 2, 8);
        for (uint64_t i = 0; i < capacity; i++)
            for (int32_t rep = 0; rep < 3; rep++)       // A head start, so nothing is at one.
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
        REQUIRE_EQ(mg.CountMonitored(), capacity);

        REQUIRE_EQ(mg.Insert(keys[capacity]), 0);
        REQUIRE_EQ(mg.CountDecrements(), 1);
        REQUIRE_EQ(mg.CountMonitored(), capacity);      // Nothing emptied...
        REQUIRE_EQ(mg.Query(keys[capacity]), 0);        // ...so nothing admitted.
        for (uint64_t i = 0; i < capacity; i++)
            REQUIRE_EQ(mg.Query(keys[i]), 2);           // Everything came down by one.
        CheckSummary(mg);
    }

    /**
     * The lazy decrement is merged out once it passes half of what a stub can
     * hold, and every count survives the rebuild it rides on.
     */
    static void TreeMergesTheLazyDecrement() {
        SublimeMG<true, true> mg(256, 34, hashmode::Default, 9);
        std::mt19937_64 rng(10);
        std::vector<uint64_t> stream;
        uint64_t merges = 0, previous = 0, widest_stub = 0;
        for (int32_t i = 0; i < 400000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(40000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
            // A merge is the only thing that makes the decrement fall.
            if (mg.GetLazyDecrement() < previous) {
                merges++;
                REQUIRE_EQ(mg.GetLazyDecrement(), 1);
                CheckSummary(mg);
                widest_stub = 0;
            }
            previous = mg.GetLazyDecrement();
            // It never climbs far past the threshold it is merged at. The
            // threshold is read against the stub of the moment, and a merge
            // can leave a narrower one behind, so the bound to hold it to is
            // the widest stub it has seen since the last merge.
            widest_stub = std::max(widest_stub,
                                   static_cast<uint64_t>(mg.Counters().GetStubLength()));
            REQUIRE_LE(mg.GetLazyDecrement(), ((uint64_t{1} << widest_stub) - 1) / 2 + 1);
        }
        REQUIRE_GT(merges, 0);
        CheckSummary(mg);
        REQUIRE_GT(CheckMisraGriesGuarantee(mg, stream), 0);
    }

    /** The tree instantiation expands under the size function like the other. */
    static void TreeExpands() {
        auto f = [](double size) { return static_cast<uint64_t>(2 * size); };
        SublimeMG<true, true> mg(64, 30, hashmode::Default, 11, 1, f);
        std::mt19937_64 rng(12);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 200000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(20000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
        }
        REQUIRE_GT(mg.CountExpansions(), 0);
        CheckSummary(mg);
        // The decrements happened while the summary was smaller, so its final
        // capacity is no bound on them; the stream length is.
        REQUIRE_GT(CheckMisraGriesGuarantee(mg, stream, 1), 0);
    }

    /** A uniform stream, where nothing is frequent and everything churns. */
    static void SurvivesAStreamWithNoHeavyHitters() {
        SublimeMG<> mg(256, 30, hashmode::Default, 39);
        std::mt19937_64 rng(40);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 100000; i++) {
            const uint64_t key = rng() % 50000;
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
            if (i % 5000 == 0) {
                CheckSummary(mg);
            }
        }
        CheckSummary(mg);
        // Nothing may be over-counted, however hard the summary churned.
        const auto truth = Frequencies(stream);
        for (const auto& [key, frequency] : truth)
            REQUIRE_LE(mg.Query(key), frequency);
        REQUIRE_LE(mg.CountDecrements() * mg.Capacity(), stream.size());
    }

    /** Resizing mid-stream keeps the guarantee, and widens the summary. */
    static void KeepsTheGuaranteeAcrossResizes() {
        SublimeMG<> mg(256, 36, hashmode::Default, 41, 2);
        std::mt19937_64 rng(42);
        std::vector<uint64_t> stream;
        const uint64_t capacity_before = mg.Capacity();
        for (int32_t i = 0; i < 200000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(40000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
            if (i > 0 && i % 40000 == 0) {
                REQUIRE_GE(mg.Expand(), 0);
                CheckSummary(mg);
            }
        }
        CheckSummary(mg);
        REQUIRE_GT(mg.Capacity(), capacity_before);
        // The decrements happened while the summary was still at its starting
        // size, so that is what bounds how many the stream could pay for.
        REQUIRE_GT(CheckMisraGriesGuarantee(mg, stream, capacity_before), 0);

        // Contracting can leave it over capacity; the next insertions bring it
        // back down rather than the contraction evicting anything itself.
        const uint64_t monitored = mg.CountMonitored();
        REQUIRE_GE(mg.Contract(), 0);
        REQUIRE_EQ(mg.CountMonitored(), monitored);
        CheckNoCountIsStranded(mg);
        for (int32_t i = 0; i < 50000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            REQUIRE_EQ(mg.Insert(static_cast<uint64_t>(40000 * u * u * u)), 0);
        }
        CheckSummary(mg);
    }

    /**
     * The count a query ought to return, worked out from the table's contents
     * rather than by scanning a run, so that it does not lean on the same
     * lookup the query does.
     */
    template <typename MGT>
    static uint64_t ExpectedQuery(const MGT& mg, uint64_t key, uint8_t flags = 0) {
        const auto& table = mg.table_;
        const uint64_t hash = table.hash_key(key, flags);
        const uint64_t bucket = table.bucket_from_hash(hash);
        const uint64_t target = table.fingerprint_from_hash(hash);

        // Every fingerprint is of the same length, so at most one entry of the
        // key's run can equal its own, and that entry's counter is the answer.
        // Worked out here by walking the table, independently of the lookup
        // the sketch itself performs.
        for (auto it = table.begin(); it != table.end(); ++it)
            if (it.bucket() == bucket && it.fingerprint() == target)
                return mg.Counters().Get(it.slot());
        return 0;
    }

    /**
     * As long as every entry carries a fingerprint of the same length -- which
     * is to say, at every size before the fingerprints run out -- a key can
     * match at most one entry, and the query has a single term. Two keys the
     * table cannot tell apart do not get an entry each: the second one finds
     * the first's entry and counts up into it. So the query agrees with the
     * count exactly, and expanding does not change that: the bucket index
     * takes over exactly the bit the fingerprint gives up, so a match is still
     * decided on the same number of bits of hash.
     */
    static void QueryAgreesWithCountWhenMatchesAreUnique() {
        SublimeMG<> mg(1024, 34, hashmode::Default, 45);
        std::mt19937_64 rng(46);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 100000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(20000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
        }
        CheckSummary(mg);

        const auto truth = Frequencies(stream);
        for (const auto& [key, frequency] : truth) {
            const uint64_t query = mg.Query(key);
            REQUIRE_EQ(query, mg.Query(key));
            REQUIRE_EQ(query, ExpectedQuery(mg, key));
            REQUIRE_LE(query, frequency);
        }

        // Still true once the table has grown and the fingerprints are shorter.
        for (uint32_t e = 1; e <= 3; e++) {
            REQUIRE_GE(mg.Expand(), 0);
            CheckNoCountIsStranded(mg);
            for (const auto& [key, frequency] : truth) {
                const uint64_t query = mg.Query(key);
                REQUIRE_EQ(query, mg.Query(key));
                REQUIRE_EQ(query, ExpectedQuery(mg, key));
            }
        }
    }

    /** Stretching changes the table's shape, and the counters follow it. */
    static void WorksUnderStretching() {
        SublimeMG<> mg(300, 26, hashmode::Default, 1, /*growth_coefficient=*/3);
        REQUIRE_EQ(mg.GetTable().GetGrowthCoefficient(), 3);
        REQUIRE_EQ(mg.Counters().CountCounters(), mg.GetTable().GetSlotCapacity());

        const auto keys = DistinctKeys(mg, 200, 19);
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> ref;
        for (size_t i = 0; i < keys.size(); i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
            for (uint64_t k = 0; k < 50 * i; k++)
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
            ref[RefKey(mg, keys[i])] = 1 + 50 * i;
        }
        CheckMirrored(mg, ref);
    }

    /* ------------------------------------------------------------------ *
     * The size function.                                                 *
     * ------------------------------------------------------------------ */

    /**
     * The threshold is what `expansion_f` returns for the size the summary
     * has now -- the stream length at which `W` outgrows it. An insertion
     * that reaches it expands, and the threshold that replaces it is the one
     * for the size that expansion produced.
     */
    static void ExpandsWhenTheSizeFunctionSaysSo() {
        // W(N) = N / 4, so a summary of C keys is outgrown at N = 4C. Counting
        // every insertion, so that the threshold is a plain stream length.
        auto f = [](double size) { return static_cast<uint64_t>(4 * size); };
        SublimeMG<false> mg(256, 30, hashmode::Default, 71, 1, f);

        const uint64_t capacity = mg.Capacity();
        REQUIRE_EQ(mg.GetExpansionLimit(), 4 * capacity);
        REQUIRE_EQ(mg.CountExpansions(), 0);

        // Counted here rather than read back from `mg`, so that the loop
        // still terminates if the summary's own count of the stream is wrong.
        for (uint64_t i = 0; i < 4 * capacity; i++) {
            REQUIRE_EQ(mg.Insert(i % 8), 0);
            REQUIRE_EQ(mg.CountExpansions(), 0);        // Not yet.
        }
        // The threshold is read before the insertion is counted, so it is the
        // next one that finds the measure has reached it.
        REQUIRE_EQ(mg.Insert(3), 0);
        REQUIRE_EQ(mg.CountExpansions(), 1);
        REQUIRE_GT(mg.Capacity(), capacity);
        REQUIRE_EQ(mg.GetExpansionLimit(), 4 * mg.Capacity());

        CheckSummary(mg);
        // The counts came across the expansion with their keys.
        REQUIRE_EQ(mg.GetStreamLength(), 4 * capacity + 1);
        uint64_t total = 0;
        for (uint64_t key = 0; key < 8; key++)
            total += mg.Query(key);
        REQUIRE_EQ(total, 4 * capacity + 1);
    }

    /** No size function means a summary of a fixed size: plain Misra-Gries. */
    static void WithoutASizeFunctionItNeverExpands() {
        SublimeMG<> mg(256, 30, hashmode::Default, 73);
        REQUIRE_EQ(mg.GetExpansionLimit(), std::numeric_limits<uint64_t>::max());

        const uint64_t capacity = mg.Capacity();
        for (uint64_t i = 0; i < 20000; i++)
            REQUIRE_EQ(mg.Insert(i % 5000), 0);

        REQUIRE_EQ(mg.CountExpansions(), 0);
        REQUIRE_EQ(mg.Capacity(), capacity);
        REQUIRE_EQ(mg.GetStreamLength(), 20000);
        REQUIRE_LE(mg.CountMonitored(), capacity);
        CheckSummary(mg);
    }

    /**
     * A size function that outgrows several of the summary's sizes at once is
     * caught up with there and then, rather than one expansion per insertion
     * from then on.
     */
    static void CatchesUpWithASteepSizeFunction() {
        // W jumps from "the size it started at" to 4000 keys at N = 5000.
        auto f = [](double size) { return size < 4000 ? uint64_t{5000}
                                        : std::numeric_limits<uint64_t>::max(); };
        SublimeMG<false> mg(256, 30, hashmode::Default, 75, 1, f);
        REQUIRE_EQ(mg.GetExpansionLimit(), 5000);

        for (uint64_t i = 0; i < 5000; i++)
            REQUIRE_EQ(mg.Insert(i % 64), 0);
        REQUIRE_EQ(mg.CountExpansions(), 0);

        // One insertion crosses every size the jump skipped over.
        REQUIRE_EQ(mg.Insert(7), 0);
        REQUIRE_GT(mg.CountExpansions(), 1);
        REQUIRE_GE(mg.Capacity(), 4000);
        REQUIRE_EQ(mg.GetExpansionLimit(), std::numeric_limits<uint64_t>::max());

        REQUIRE_EQ(mg.GetStreamLength(), 5001);
        CheckSummary(mg);
    }

    /**
     * A size function that is never satisfied would expand the summary until
     * the machine ran out of memory. It is capped at a slot per element of the
     * stream, which is where Misra-Gries becomes exact counting and no size
     * function can ask for more.
     */
    static void NeverGrowsPastTheStream() {
        auto f = [](double) { return uint64_t{0}; };   // Always too small.
        SublimeMG<false> mg(64, 30, hashmode::Default, 83, 1, f);
        REQUIRE_EQ(mg.GetExpansionLimit(), 0);
        const uint64_t capacity_before = mg.Capacity();

        for (uint64_t i = 0; i < 2000; i++) {
            REQUIRE_EQ(mg.Insert(i % 32), 0);
            // It expands only while it holds fewer slots than the stream has
            // elements, and an expansion at most doubles it, so it stays under
            // twice the stream however loudly the size function asks.
            REQUIRE_LE(mg.Capacity(), std::max(capacity_before, 2 * (i + 1)));
        }
        REQUIRE_GT(mg.CountExpansions(), 0);
        REQUIRE_LT(mg.Capacity(), 4000);
        CheckSummary(mg);
    }

    /**
     * A table whose fingerprints are down to their last bit cannot expand
     * however loudly the size function asks -- expanding would spend a bit it
     * does not have -- so the threshold is retired rather than tested once per
     * insertion for the rest of the stream.
     */
    static void StopsTestingWhenItCannotGrow() {
        auto f = [](double) { return uint64_t{1}; };
        // 9 key bits over 256 slots leaves a 1-bit fingerprint, and expanding
        // would take that bit for the bucket index.
        SublimeMG<> mg(256, 9, hashmode::Default, 77, 1, f);
        REQUIRE_EQ(mg.GetTable().GetNumFingerprintBits(), 1);
        REQUIRE_EQ(mg.GetTable().CountSlotsAfterExpansion(), mg.GetTable().CountSlots());
        REQUIRE_EQ(mg.GetExpansionLimit(), std::numeric_limits<uint64_t>::max());

        for (uint64_t i = 0; i < 500; i++)
            REQUIRE_EQ(mg.Insert(i % 200), 0);
        REQUIRE_EQ(mg.CountExpansions(), 0);
        CheckSummary(mg);
    }

    /**
     * The point of the whole policy: a stream that keeps growing grows the
     * summary with it, without the caller resizing anything, and the
     * Misra-Gries guarantee survives the expansions it decides on.
     */
    static void KeepsTheGuaranteeWhileTheSizeFunctionDrivesIt() {
        // W(N) = sqrt(N) / 2, whose inverse is `expansion_f(C) = (2C)^2`.
        auto f = [](double size) { return static_cast<uint64_t>(4 * size * size); };
        SublimeMG<> mg(128, 34, hashmode::Default, 79, /*growth_coefficient=*/2, f);
        const uint64_t capacity_before = mg.Capacity();

        std::mt19937_64 rng(79);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 300000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(60000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
        }
        CheckSummary(mg);

        REQUIRE_GT(mg.CountExpansions(), 1);
        REQUIRE_GT(mg.Capacity(), capacity_before);
        REQUIRE_LE(mg.CountMonitored(), mg.Capacity());
        // The summary is never left smaller than the size function asks for.
        REQUIRE_GE(mg.GetExpansionLimit(), mg.SizeMeasure());
        // The decrements were paid for while it was still at its starting
        // size, so that is what bounds how many the stream could afford.
        REQUIRE_GT(CheckMisraGriesGuarantee(mg, stream, capacity_before), 0);
    }

    /**
     * An insertion is error-inducing when nothing in the table matched it. A
     * key the summary already holds is counted for free -- that occurrence
     * lands on a counter that was already there, exactly as an exact counter
     * would have handled it.
     */
    static void CountsOnlyTheInsertionsThatMissed() {
        SublimeMG<> mg(256, 30, hashmode::Default, 85);
        REQUIRE_EQ(mg.CountErrorInducingInsertions(), 0);

        // Only the first occurrence of a key misses; the other 99 land on it.
        for (uint64_t i = 0; i < 100; i++)
            REQUIRE_EQ(mg.Insert(7), 0);
        REQUIRE_EQ(mg.GetStreamLength(), 100);
        REQUIRE_EQ(mg.CountErrorInducingInsertions(), 1);

        // Fifty keys the table can tell apart miss once each ...
        const auto keys = DistinctKeys(mg, 50, 85);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Insert(key), 0);
        REQUIRE_EQ(mg.CountErrorInducingInsertions(), 51);

        // ... and never again, however many times they come back.
        for (uint64_t round = 0; round < 20; round++)
            for (const uint64_t key : keys)
                REQUIRE_EQ(mg.Insert(key), 0);
        REQUIRE_EQ(mg.GetStreamLength(), 100 + 21 * 50);
        REQUIRE_EQ(mg.CountErrorInducingInsertions(), 51);
        REQUIRE_EQ(mg.SizeMeasure(), 51);           // What the threshold reads.
        CheckSummary(mg);
    }

    /**
     * A summary nothing has overflowed answers exactly, and expanding it would
     * buy nothing. Under the default measure it cannot: the cap in
     * `grow_to_fit` sits at one slot per error-inducing insertion, and a
     * summary that has room for every key it has missed already has one --
     * even with a size function shouting for more.
     */
    static void AnExactSummaryNeverExpands() {
        auto f = [](double) { return uint64_t{10}; };   // Always asks to grow.
        SublimeMG<> mg(256, 30, hashmode::Default, 87, 1, f);
        REQUIRE_EQ(mg.GetExpansionLimit(), 10);

        const auto keys = DistinctKeys(mg, 50, 87);
        for (uint64_t round = 0; round < 400; round++)
            for (const uint64_t key : keys)
                REQUIRE_EQ(mg.Insert(key), 0);

        // The threshold really is exceeded -- it is the cap that holds it.
        REQUIRE_GE(mg.SizeMeasure(), mg.GetExpansionLimit());
        REQUIRE_EQ(mg.CountExpansions(), 0);
        REQUIRE_EQ(mg.GetStreamLength(), 400 * 50);
        REQUIRE_EQ(mg.CountErrorInducingInsertions(), 50);
        // Nothing was ever evicted, so every count is the true one.
        REQUIRE_EQ(mg.CountDecrements(), 0);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), 400);
        CheckSummary(mg);
    }

    /**
     * The two measures side by side on one skewed stream: counting every
     * insertion grows the summary far past what counting only the ones that
     * missed asks for, because a heavy hitter misses once and is then free.
     */
    static void TheTwoMeasuresDisagreeOnASkewedStream() {
        auto f = [](double size) { return static_cast<uint64_t>(4 * size); };
        SublimeMG<> selective(256, 34, hashmode::Default, 89, 1, f);
        SublimeMG<false> everything(256, 34, hashmode::Default, 89, 1, f);

        std::mt19937_64 rng(89);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 200000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(40000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(selective.Insert(key), 0);
            REQUIRE_EQ(everything.Insert(key), 0);
        }
        CheckSummary(selective);
        CheckSummary(everything);

        REQUIRE_EQ(selective.GetStreamLength(), everything.GetStreamLength());
        REQUIRE_EQ(selective.SizeMeasure(), selective.CountErrorInducingInsertions());
        REQUIRE_EQ(everything.SizeMeasure(), everything.GetStreamLength());
        // The skew is the whole point: most occurrences land on a key the
        // summary already had, and cost it nothing.
        REQUIRE_LT(selective.SizeMeasure(), selective.GetStreamLength() / 2);

        // Both grew, but the selective one grew to what the stream actually
        // asked of it rather than to how long the stream was.
        REQUIRE_GT(selective.CountExpansions(), 0);
        REQUIRE_LT(selective.CountExpansions(), everything.CountExpansions());
        REQUIRE_LT(selective.Capacity(), everything.Capacity());
        REQUIRE_LT(selective.SizeInBytes(), everything.SizeInBytes());

        // And both still answer within the Misra-Gries guarantee.
        CheckMisraGriesGuarantee(selective, stream, 256 / 2);
        CheckMisraGriesGuarantee(everything, stream, 256 / 2);
    }

    /** `Reset` puts the stream length back to zero along with the counts. */
    static void ResetRestartsTheStream() {
        auto f = [](double size) { return static_cast<uint64_t>(4 * size); };
        SublimeMG<false> mg(256, 30, hashmode::Default, 81, 1, f);
        for (uint64_t i = 0; i < 4000; i++)
            REQUIRE_EQ(mg.Insert(i % 64), 0);
        REQUIRE_GT(mg.CountExpansions(), 0);

        const uint64_t capacity = mg.Capacity();
        mg.Reset();
        REQUIRE_EQ(mg.GetStreamLength(), 0);
        REQUIRE_EQ(mg.CountMonitored(), 0);
        // Reset keeps the size the summary grew to, and so its threshold.
        REQUIRE_EQ(mg.Capacity(), capacity);
        REQUIRE_EQ(mg.GetExpansionLimit(), 4 * capacity);
        CheckSummary(mg);
    }
};

}   // namespace sublime

using sublime::SublimeMGTest;

TEST_SUITE("SublimeMG") {
    TEST_CASE("insert increments the match") {
        SublimeMGTest::InsertIncrementsTheMatch();
    }

    TEST_CASE("counts follow their fingerprints") {
        SublimeMGTest::CountsFollowTheirFingerprints();
    }

    TEST_CASE("counts survive long clusters") {
        SublimeMGTest::CountsSurviveLongClusters();
    }

    TEST_CASE("monte carlo") {
        SublimeMGTest::MonteCarlo();
    }

    TEST_CASE("retunes under large counts") {
        SublimeMGTest::RetunesUnderLargeCounts();
    }

    TEST_CASE("expansion carries counts") {
        SublimeMGTest::ExpansionCarriesCounts();
    }

    TEST_CASE("contraction restores counts") {
        SublimeMGTest::ContractionRestoresCounts();
    }

    TEST_CASE("resize monte carlo") {
        SublimeMGTest::ResizeMonteCarlo();
    }

    TEST_CASE("copy, move, and reset") {
        SublimeMGTest::CopyMoveAndReset();
    }

    TEST_CASE("query agrees with count when matches are unique") {
        SublimeMGTest::QueryAgreesWithCountWhenMatchesAreUnique();
    }

    TEST_CASE("works under stretching") {
        SublimeMGTest::WorksUnderStretching();
    }

    TEST_CASE("expands when the size function says so") {
        SublimeMGTest::ExpandsWhenTheSizeFunctionSaysSo();
    }

    TEST_CASE("without a size function it never expands") {
        SublimeMGTest::WithoutASizeFunctionItNeverExpands();
    }

    TEST_CASE("catches up with a steep size function") {
        SublimeMGTest::CatchesUpWithASteepSizeFunction();
    }

    TEST_CASE("never grows past the stream") {
        SublimeMGTest::NeverGrowsPastTheStream();
    }

    TEST_CASE("stops testing when it cannot grow") {
        SublimeMGTest::StopsTestingWhenItCannotGrow();
    }

    TEST_CASE("keeps the guarantee while the size function drives it") {
        SublimeMGTest::KeepsTheGuaranteeWhileTheSizeFunctionDrivesIt();
    }

    TEST_CASE("counts only the insertions that missed") {
        SublimeMGTest::CountsOnlyTheInsertionsThatMissed();
    }

    TEST_CASE("an exact summary never expands") {
        SublimeMGTest::AnExactSummaryNeverExpands();
    }

    TEST_CASE("the two measures disagree on a skewed stream") {
        SublimeMGTest::TheTwoMeasuresDisagreeOnASkewedStream();
    }

    TEST_CASE("reset restarts the stream") {
        SublimeMGTest::ResetRestartsTheStream();
    }

    TEST_CASE("admits while there is room") {
        SublimeMGTest::AdmitsWhileThereIsRoom();
    }

    TEST_CASE("evicts the smallest counters") {
        SublimeMGTest::EvictsTheSmallestCounters();
    }

    TEST_CASE("many tied counters empty at once") {
        SublimeMGTest::ManyTiedCountersEmptyAtOnce();
    }

    TEST_CASE("keeps the misra-gries guarantee") {
        SublimeMGTest::KeepsTheMisraGriesGuarantee();
    }

    TEST_CASE("min tree monte carlo") {
        SublimeMGTest::TreeMonteCarlo(/*nslots=*/256, /*universe=*/20000, /*steps=*/200000, 21);
        SublimeMGTest::TreeMonteCarlo(1024, 50000, 200000, 22);
        SublimeMGTest::TreeMonteCarlo(64, 4000, 100000, 23);
    }

    TEST_CASE("cuckoo table, sweep and tree") {
        // The same streams, over a cuckoo filter instead of a quotient filter.
        // Neither sketch knows which table it has, so every invariant the
        // suite checks has to hold over both.
        using Sweep = sublime::SublimeMG<true, false, sublime::CuckooTable>;
        using Tree = sublime::SublimeMG<true, true, sublime::CuckooTable>;
        SublimeMGTest::TreeMonteCarlo<Sweep>(/*nslots=*/256, /*universe=*/20000,
                                             /*steps=*/200000, 31);
        SublimeMGTest::TreeMonteCarlo<Tree>(256, 20000, 200000, 32);
        SublimeMGTest::TreeMonteCarlo<Sweep>(1024, 50000, 200000, 33);
        SublimeMGTest::TreeMonteCarlo<Tree>(1024, 50000, 200000, 34);
    }

    TEST_CASE("min tree evicts every emptied key at once") {
        SublimeMGTest::TreeEvictsEveryEmptiedKeyAtOnce();
    }

    TEST_CASE("min tree drops an arrival that frees nothing") {
        SublimeMGTest::TreeDropsAnArrivalThatFreesNothing();
    }

    TEST_CASE("min tree merges the lazy decrement") {
        SublimeMGTest::TreeMergesTheLazyDecrement();
    }

    TEST_CASE("min tree expands") {
        SublimeMGTest::TreeExpands();
    }

    TEST_CASE("survives a stream with no heavy hitters") {
        SublimeMGTest::SurvivesAStreamWithNoHeavyHitters();
    }

    TEST_CASE("agrees with plain Misra-Gries exactly") {
        SublimeMGTest::AgreesWithPlainMisraGries();
    }

    TEST_CASE("keeps the guarantee across resizes") {
        SublimeMGTest::KeepsTheGuaranteeAcrossResizes();
    }
}
