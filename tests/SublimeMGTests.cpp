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
    template <bool E>
    static void CheckMirrored(const SublimeMG<E>& mg,
                              const std::map<std::pair<uint64_t, uint64_t>, uint64_t>& ref) {
        const FingerprintTable& table = mg.table_;
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
    template <bool E>
    static std::pair<uint64_t, uint64_t> RefKey(const SublimeMG<E>& mg, uint64_t key) {
        const uint64_t hash = mg.table_.hash_key(key, 0);
        return {mg.table_.bucket_from_hash(hash),
                mg.table_.fingerprint_from_hash(hash, mg.table_.GetNumFingerprintBits())};
    }

    /**
     * @returns `count` keys that all land on distinct entries, so that a
     * reference model keyed by the entry is exact rather than approximate.
     */
    template <bool E>
    static std::vector<uint64_t> DistinctKeys(const SublimeMG<E>& mg, uint64_t count, uint64_t seed) {
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
     * to changes and a void entry may have been duplicated: every slot the
     * table calls occupied carries a count of at least one -- every monitored
     * key was admitted at one and only ever counted up -- and every slot it
     * calls empty carries nothing at all.
     */
    template <bool E>
    static void CheckNoCountIsStranded(const SublimeMG<E>& mg) {
        const FingerprintTable& table = mg.table_;
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
    template <bool E>
    static std::vector<uint64_t> StoredCounts(const SublimeMG<E>& mg) {
        std::vector<uint64_t> res;
        for (auto it = mg.table_.begin(); it != mg.table_.end(); ++it)
            res.push_back(mg.Counters().Get(it.slot()));
        return res;
    }

    /**
     * Every entry of the table, with the chain sum a query matching it would
     * add up: its own counter plus the counters of the earlier entries of its
     * run that its fingerprint extends. Worked out here by comparing stored
     * lengths, independently of the table's own sweep.
     */
    template <bool E>
    static std::vector<std::pair<uint64_t, uint64_t>> ChainSums(const SublimeMG<E>& mg) {
        struct Entry { uint64_t bucket, fingerprint, count; };
        std::vector<Entry> entries;
        for (auto it = mg.table_.begin(); it != mg.table_.end(); ++it)
            entries.push_back({it.bucket(), it.fingerprint(), mg.Counters().Get(it.slot())});

        std::vector<std::pair<uint64_t, uint64_t>> res;   // (own counter, chain sum)
        for (size_t j = 0; j < entries.size(); j++) {
            uint64_t sum = entries[j].count;
            for (size_t i = 0; i < j; i++) {
                if (entries[i].bucket != entries[j].bucket)
                    continue;
                uint32_t length = 0;
                while ((entries[i].fingerprint >> (length + 1)) != 0)
                    length++;
                const uint64_t mask = (1ULL << length) - 1;
                if ((entries[i].fingerprint & mask) == (entries[j].fingerprint & mask))
                    sum += entries[i].count;
            }
            res.push_back({entries[j].count, sum});
        }
        return res;
    }

    /**
     * What has to hold of the summary between operations, on top of the
     * mirroring: every entry's chain sum stands strictly above the lazy
     * decrement -- so the count of the key family ending there is at least one
     * -- the running sum the merge policy is driven from agrees with the
     * counters themselves, and no more keys are monitored than the summary has
     * room for.
     *
     * Note that an individual counter may well sit *below* the lazy decrement:
     * an entry admitted onto a chain that already carries it holds only its
     * own count. It is the chain that owes the decrement, not the entry.
     */
    template <bool E>
    static void CheckSummary(const SublimeMG<E>& mg) {
        CheckNoCountIsStranded(mg);
        uint64_t sum = 0, entries = 0;
        for (const auto& [own, chain_sum] : ChainSums(mg)) {
            REQUIRE_GT(chain_sum, mg.lazy_decrement_);
            sum += own;
            entries++;
        }
        REQUIRE_EQ(sum, mg.counter_sum_);
        REQUIRE_EQ(entries, mg.CountMonitored());
        REQUIRE_LE(mg.CountMonitored(), mg.Capacity());
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
    template <bool E>
    static uint64_t CheckMisraGriesGuarantee(const SublimeMG<E>& mg,
                                             const std::vector<uint64_t>& stream,
                                             uint64_t capacity_bound = 0) {
        const auto truth = Frequencies(stream);
        const uint64_t decrements = mg.CountDecrements();
        uint64_t heavy = 0;
        for (const auto& [key, frequency] : truth) {
            const uint64_t estimate = mg.Query(key);
            REQUIRE_LE(estimate, frequency);
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

    static void InsertIncrementsTheLongestMatch() {
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
                if (mg.CountMonitored() + 1 >= mg.Table().CountSlots() * FingerprintTable::max_load_factor)
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
                REQUIRE_EQ(mg.Expand(), keys.size());   // No void entries at this size.
                CheckNoCountIsStranded(mg);
                REQUIRE_EQ(mg.CountMonitored(), keys.size());
                REQUIRE_EQ(mg.Counters().CountCounters(), mg.Table().GetSlotCapacity());
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

    /**
     * A void entry has no fingerprint bit left to say which of the two buckets
     * it now belongs to, so it goes into both -- and both copies keep the
     * whole count, since a query may land on either and an under-estimate is
     * the one thing a Misra-Gries count must never be.
     */
    static void VoidEntriesKeepTheWholeCountTwice() {
        SublimeMG<> mg(64, 14, hashmode::Default, 3);
        const uint64_t full_length = mg.Table().GetNumFingerprintBits();
        REQUIRE_EQ(full_length, 8);

        const uint64_t key = 0xC0FFEE;
        REQUIRE_EQ(mg.StartMonitoring(key), 0);
        const uint64_t count = 40000;   // Long enough to need an extension.
        for (uint64_t i = 1; i < count; i++)
            REQUIRE_EQ(mg.Insert(key), 0);
        REQUIRE_EQ(mg.Query(key), count);

        // Spend the fingerprint one bit at a time. Until it runs out, the
        // entry stays single and keeps its count.
        for (uint64_t e = 1; e <= full_length; e++) {
            REQUIRE_EQ(mg.Expand(), 1);
            CheckNoCountIsStranded(mg);
            REQUIRE_EQ(mg.CountMonitored(), 1);
            REQUIRE_EQ(mg.Table().MatchLength(key), full_length - e);
            REQUIRE_EQ(mg.Query(key), count);
            REQUIRE(StoredCounts(mg) == std::vector<uint64_t>{count});
        }

        // From here on every expansion doubles the entries, and every copy
        // carries the full count.
        uint64_t expected = 1;
        for (uint32_t e = 1; e <= 3; e++) {
            expected *= 2;
            REQUIRE_EQ(mg.Expand(), expected);
            CheckNoCountIsStranded(mg);
            REQUIRE_EQ(mg.CountMonitored(), expected);
            REQUIRE_EQ(mg.Query(key), count);
            REQUIRE(StoredCounts(mg) == std::vector<uint64_t>(expected, count));
        }

        // Counting the key again finds one copy and raises it; the answer
        // stays an over-estimate of the truth either way.
        REQUIRE_EQ(mg.Insert(key), 0);
        REQUIRE_EQ(mg.Query(key), count + 1);
        CheckNoCountIsStranded(mg);
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
            const uint64_t slots_before = mg.Table().CountSlots();

            const uint32_t steps = 2 * r + 1;
            for (uint32_t i = 0; i < steps; i++)
                REQUIRE_EQ(mg.Expand(), keys.size());
            for (uint32_t i = 0; i < steps; i++) {
                REQUIRE_EQ(mg.Contract(), keys.size());
                CheckNoCountIsStranded(mg);
                for (const auto& [key, count] : ref)
                    REQUIRE_EQ(mg.Query(key), count);
            }
            REQUIRE_EQ(mg.Table().CountSlots(), slots_before);
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
            if (action < 3 && mg.Table().GetNumKeyBits() < 44) {
                REQUIRE_GE(mg.Expand(), 0);
                CheckNoCountIsStranded(mg);
            }
            else if (action < 6 && mg.Table().GetExpansionCount() > 0
                     && mg.CountMonitored() < mg.Table().CountSlots() / 2
                                                * FingerprintTable::max_load_factor) {
                REQUIRE_GE(mg.Contract(), 0);
                CheckNoCountIsStranded(mg);
            }
            else if (action < 20 || monitored.empty()) {
                const uint64_t key = keys[rng() % keys.size()];
                if (mg.IsMonitored(key))
                    continue;
                if (mg.CountMonitored() + 1 >= mg.Table().CountSlots()
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
        SublimeMG<> mg(128, 26, hashmode::Default, 1, 1, /*buffer_capacity=*/8);
        const uint64_t capacity = mg.Capacity();
        REQUIRE_EQ(capacity, static_cast<uint64_t>(128 * FingerprintTable::max_load_factor));

        const auto keys = DistinctKeys(mg, capacity, 31);
        for (uint64_t i = 0; i < capacity; i++) {
            REQUIRE_EQ(mg.Insert(keys[i]), 0);
            REQUIRE_EQ(mg.CountMonitored(), i + 1);
            REQUIRE_EQ(mg.Query(keys[i]), 1);
            // Nothing is buffered and nothing is decremented while there is room.
            REQUIRE_EQ(mg.CountBuffered(), 0);
            REQUIRE_EQ(mg.CountDecrements(), 0);
        }
        CheckSummary(mg);
        // A second occurrence of a monitored key just counts up.
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Insert(key), 0);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), 2);
        REQUIRE_EQ(mg.CountBuffered(), 0);
        CheckSummary(mg);
    }

    /** Once it is full, an unmonitored key waits in the buffer. */
    static void BuffersWhenFull() {
        const uint64_t buffer_capacity = 8;
        SublimeMG<> mg(128, 26, hashmode::Default, 1, 1, buffer_capacity);
        const auto keys = DistinctKeys(mg, mg.Capacity() + buffer_capacity, 32);
        for (uint64_t i = 0; i < mg.Capacity(); i++)
            REQUIRE_EQ(mg.Insert(keys[i]), 0);
        const uint64_t monitored = mg.CountMonitored();

        // The next few go into the buffer and change nothing yet.
        for (uint64_t i = 0; i < buffer_capacity - 1; i++) {
            REQUIRE_EQ(mg.Insert(keys[monitored + i]), 0);
            REQUIRE_EQ(mg.CountBuffered(), i + 1);
            REQUIRE_EQ(mg.CountMonitored(), monitored);
            REQUIRE_EQ(mg.CountDecrements(), 0);
            // A buffered insertion is not visible until it is applied.
            REQUIRE_EQ(mg.Query(keys[monitored + i]), 0);
        }

        // The one that fills the buffer applies the whole batch. Every count
        // here is one, so the first decrement alone empties the summary and
        // the rest of the batch moves into the room that makes -- no more
        // decrements needed.
        REQUIRE_EQ(mg.Insert(keys[monitored + buffer_capacity - 1]), 0);
        REQUIRE_EQ(mg.CountBuffered(), 0);
        REQUIRE_EQ(mg.CountDecrements(), 1);
        REQUIRE_EQ(mg.CountMonitored(), buffer_capacity);
        CheckSummary(mg);

        // Flushing an empty buffer is a no-op.
        REQUIRE_EQ(mg.FlushBuffer(), 0);
        CheckSummary(mg);
    }

    /**
     * The batch evicts exactly the smallest counters, in order, and hands each
     * freed slot to the buffered key whose decrement emptied it -- and a key
     * that took a slot is itself decremented by whatever follows it in the
     * batch, exactly as Misra-Gries would have it.
     *
     * That last part is what makes the arithmetic here interesting. Against a
     * summary holding counts 1, 2, 3, ... the first arrival decrements once
     * and takes the slot the count of one leaves. The second decrements again,
     * which frees the count of two *and* the first arrival -- two slots, so it
     * and the third arrival both get in. The next decrement frees three, and
     * so on: the arrivals admitted between two decrements all die at the next
     * one, so a batch of `B` keys spends its decrements in a triangular
     * pattern, `1 + 2 + 3 + ...` keys per decrement, and only the arrivals
     * after the last decrement are still standing at the end of it.
     */
    static void EvictsTheSmallestCounters() {
        const uint64_t buffer_capacity = 8;
        SublimeMG<> mg(128, 30, hashmode::Default, 1, 1, buffer_capacity);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + buffer_capacity, 33);

        // Counts 1, 2, 3, ... so that the order the evictions come in is known.
        for (uint64_t i = 0; i < capacity; i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
            for (uint64_t k = 1; k <= i; k++)
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
            REQUIRE_EQ(mg.Query(keys[i]), i + 1);
        }
        REQUIRE_EQ(mg.CountMonitored(), capacity);

        // Fill the buffer with keys the summary has never seen.
        for (uint64_t i = 0; i < buffer_capacity; i++)
            REQUIRE_EQ(mg.Insert(keys[capacity + i]), 0);
        REQUIRE_EQ(mg.CountBuffered(), 0);   // The last one flushed it.
        CheckSummary(mg);

        // 1 + 2 + 3 = 6 of the eight keys were spent on four decrements, the
        // fourth of which had room for four and only two left to put in it.
        const uint64_t decrements = 4;
        const uint64_t survivors = 2;
        REQUIRE_EQ(mg.CountDecrements(), decrements);

        // The count-1 through count-`decrements` entries are the ones it
        // reached, and everything above them came down by exactly that many.
        for (uint64_t i = 0; i < decrements; i++)
            REQUIRE_FALSE(mg.IsMonitored(keys[i]));
        for (uint64_t i = decrements; i < capacity; i++)
            REQUIRE_EQ(mg.Query(keys[i]), i + 1 - decrements);

        // Of the batch, only the keys that arrived after the last decrement
        // are still there; the earlier ones were admitted and decremented back
        // out again before the batch was through.
        for (uint64_t i = 0; i + survivors < buffer_capacity; i++)
            REQUIRE_FALSE(mg.IsMonitored(keys[capacity + i]));
        for (uint64_t i = buffer_capacity - survivors; i < buffer_capacity; i++)
            REQUIRE_EQ(mg.Query(keys[capacity + i]), 1);
        REQUIRE_EQ(mg.CountMonitored(), capacity - decrements + survivors);
    }

    /**
     * A batch large enough to zero more counters than one pass looks at. The
     * pass only collects the `B` smallest, so the sweep that follows the
     * replay -- which takes every entry the decrement has caught up with, seen
     * or not -- is what keeps entries from being left standing at zero.
     */
    static void ManyTiedCountersEmptyAtOnce() {
        const uint64_t buffer_capacity = 8;
        SublimeMG<> mg(256, 30, hashmode::Default, 1, 1, buffer_capacity);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + buffer_capacity, 34);

        // Every single count is one, so the first decrement zeroes all of them
        // at once -- far more than the `B` the pass collects.
        for (uint64_t i = 0; i < capacity; i++)
            REQUIRE_EQ(mg.Insert(keys[i]), 0);
        REQUIRE_EQ(mg.CountMonitored(), capacity);

        for (uint64_t i = 0; i < buffer_capacity; i++)
            REQUIRE_EQ(mg.Insert(keys[capacity + i]), 0);
        CheckSummary(mg);

        // One decrement emptied the summary, and the buffered keys moved in.
        REQUIRE_EQ(mg.CountDecrements(), 1);
        for (uint64_t i = 0; i < capacity; i++)
            REQUIRE_FALSE(mg.IsMonitored(keys[i]));
        REQUIRE_EQ(mg.CountMonitored(), buffer_capacity);
        for (uint64_t i = 0; i < buffer_capacity; i++)
            REQUIRE_EQ(mg.Query(keys[capacity + i]), 1);
    }

    /**
     * A count reads as the stored value less the lazy decrement, and merging
     * the one into the other changes no count at all.
     */
    static void LazyDecrementAndMerging() {
        const uint64_t buffer_capacity = 4;
        SublimeMG<> mg(128, 30, hashmode::Default, 1, 1, buffer_capacity);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + 200, 35);

        for (uint64_t i = 0; i < capacity; i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
            for (uint64_t k = 0; k < 200 + i; k++)
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
        }
        // Nothing has been decremented, so nothing is owed.
        REQUIRE_EQ(mg.GetLazyDecrement(), 0);
        CheckSummary(mg);

        // Drive the lazy counter up without letting the automatic merge fire:
        // the counts here are in the hundreds, so it takes a while.
        for (uint64_t i = 0; i < 100; i++)
            REQUIRE_EQ(mg.Insert(keys[capacity + i]), 0);
        mg.FlushBuffer();
        REQUIRE_GT(mg.GetLazyDecrement(), 0);
        CheckSummary(mg);

        // Every count is the stored value less what is owed.
        std::map<uint64_t, uint64_t> counts;
        for (auto it = mg.table_.begin(); it != mg.table_.end(); ++it)
            REQUIRE_EQ(mg.Counters().Get(it.slot()) - mg.GetLazyDecrement(),
                       mg.Counters().Get(it.slot()) - mg.lazy_decrement_);
        for (uint64_t i = 0; i < capacity + 100; i++)
            if (mg.IsMonitored(keys[i]))
                counts[keys[i]] = mg.Query(keys[i]);
        const uint64_t owed = mg.GetLazyDecrement();
        const auto stored_before = StoredCounts(mg);

        // Merging subtracts it out of every counter and leaves the counts alone.
        mg.MergeLazyDecrement();
        REQUIRE_EQ(mg.GetLazyDecrement(), 0);
        CheckSummary(mg);
        for (const auto& [key, count] : counts)
            REQUIRE_EQ(mg.Query(key), count);
        const auto stored_after = StoredCounts(mg);
        REQUIRE_EQ(stored_after.size(), stored_before.size());
        for (size_t i = 0; i < stored_after.size(); i++)
            REQUIRE_EQ(stored_after[i] + owed, stored_before[i]);

        // Merging again is a no-op.
        mg.MergeLazyDecrement();
        REQUIRE(StoredCounts(mg) == stored_after);
    }

    /**
     * The automatic merge fires once the lazy counter overtakes the mean of
     * the counts, and never leaves a count wrong.
     */
    static void MergesOnItsOwn() {
        // Counts stay small here, so the lazy counter overtakes them quickly.
        SublimeMG<> mg(256, 30, hashmode::Default, 1, 1, 16);
        std::mt19937_64 rng(36);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 60000; i++) {
            const uint64_t key = rng() % 4000;
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
        }
        mg.FlushBuffer();
        CheckSummary(mg);

        // It has been merging all along: the lazy counter never ran away with
        // itself, even though far more decrements than that were applied.
        REQUIRE_GT(mg.CountDecrements(), 0);
        REQUIRE_LT(mg.GetLazyDecrement(), mg.CountDecrements());
        // Nothing in this stream is frequent enough for the guarantee to
        // promise it a count, but it still may not be over-counted.
        CheckMisraGriesGuarantee(mg, stream);
    }

    /**
     * A retune reads and rewrites every counter, which is exactly what merging
     * the lazy decrement takes -- so a retune always clears it, whether or not
     * the merge's own trigger was anywhere near firing.
     */
    static void RetuningAppliesTheLazyDecrement() {
        const uint64_t buffer_capacity = 4;
        SublimeMG<> mg(256, 34, hashmode::Default, 43, 1, buffer_capacity);
        const uint64_t capacity = mg.Capacity();
        const auto keys = DistinctKeys(mg, capacity + buffer_capacity, 44);

        // Fill it, with counts in the thousands.
        for (uint64_t i = 0; i < capacity; i++) {
            REQUIRE_EQ(mg.StartMonitoring(keys[i]), 0);
            for (uint64_t k = 1; k < 2000; k++)
                REQUIRE_EQ(mg.Insert(keys[i]), 0);
        }
        // Put something on the lazy decrement. Against counts this large its
        // own trigger is nowhere near firing, so nothing will merge it.
        for (uint64_t i = 0; i < buffer_capacity; i++)
            REQUIRE_EQ(mg.Insert(keys[capacity + i]), 0);
        mg.FlushBuffer();
        REQUIRE_GT(mg.GetLazyDecrement(), 0);
        REQUIRE_FALSE(mg.lazy_decrement_is_dead_weight());
        CheckSummary(mg);

        // Whatever the eviction left standing.
        std::vector<uint64_t> survivors;
        std::vector<uint64_t> expected;
        for (uint64_t i = 0; i < capacity; i++) {
            if (!mg.IsMonitored(keys[i]))
                continue;
            survivors.push_back(keys[i]);
            expected.push_back(mg.Query(keys[i]));
        }
        REQUIRE_GT(survivors.size(), 0);
        const uint32_t stub_before = mg.Counters().GetStubLength();

        // Now count the survivors up hard. Every one of them is monitored, so
        // these are all case-1 insertions: nothing is buffered, nothing is
        // decremented, and the lazy decrement cannot move. The only thing that
        // changes is that the counts outgrow VALE's tuning.
        const uint64_t decrements_before = mg.CountDecrements();
        uint64_t rounds = 0;
        while (mg.Counters().GetStubLength() == stub_before) {
            for (size_t i = 0; i < survivors.size(); i++) {
                for (int32_t k = 0; k < 200; k++)
                    REQUIRE_EQ(mg.Insert(survivors[i]), 0);
                expected[i] += 200;
            }
            REQUIRE_LT(++rounds, 200);      // It has to happen sooner than this.
        }
        REQUIRE_EQ(mg.CountDecrements(), decrements_before);

        // The retune took the lazy decrement with it, and left every count
        // exactly where it was.
        REQUIRE_EQ(mg.GetLazyDecrement(), 0);
        CheckSummary(mg);
        for (size_t i = 0; i < survivors.size(); i++)
            REQUIRE_EQ(mg.Query(survivors[i]), expected[i]);
    }

    /** The guarantee itself, on a skewed stream, at several buffer sizes. */
    static void KeepsTheMisraGriesGuarantee() {
        for (const uint64_t buffer_capacity : {1ULL, 4ULL, 64ULL, 512ULL}) {
            SublimeMG<> mg(1024, 34, hashmode::Default, 37, 1, buffer_capacity);
            std::mt19937_64 rng(38);
            std::vector<uint64_t> stream;
            for (int32_t i = 0; i < 300000; i++) {
                // Skewed: a few keys dominate, with a long tail behind them.
                const double u = (rng() % 1000000) / 1000000.0;
                const uint64_t key = static_cast<uint64_t>(50000 * u * u * u * u);
                stream.push_back(key);
                REQUIRE_EQ(mg.Insert(key), 0);
            }
            mg.FlushBuffer();
            CheckSummary(mg);
            REQUIRE_GT(CheckMisraGriesGuarantee(mg, stream), 0);
        }
    }

    /** A uniform stream, where nothing is frequent and everything churns. */
    static void SurvivesAStreamWithNoHeavyHitters() {
        SublimeMG<> mg(256, 30, hashmode::Default, 39, 1, 32);
        std::mt19937_64 rng(40);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 100000; i++) {
            const uint64_t key = rng() % 50000;
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
            if (i % 5000 == 0) {
                mg.FlushBuffer();
                CheckSummary(mg);
            }
        }
        mg.FlushBuffer();
        CheckSummary(mg);
        // Nothing may be over-counted, however hard the summary churned.
        const auto truth = Frequencies(stream);
        for (const auto& [key, frequency] : truth)
            REQUIRE_LE(mg.Query(key), frequency);
        REQUIRE_LE(mg.CountDecrements() * mg.Capacity(), stream.size());
    }

    /** Resizing mid-stream keeps the guarantee, and widens the summary. */
    static void KeepsTheGuaranteeAcrossResizes() {
        SublimeMG<> mg(256, 36, hashmode::Default, 41, 2, 32);
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
        mg.FlushBuffer();
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
        mg.FlushBuffer();
        CheckSummary(mg);
    }

    /** @returns How many stored fingerprints a query for `key` sums up. */
    template <bool E>
    static uint64_t MatchCount(const SublimeMG<E>& mg, uint64_t key, uint8_t flags = 0) {
        return mg.table_.SumMatchingCounters(key, flags).count;
    }

    /**
     * The sum a query ought to return, worked out entry by entry from the
     * table's contents rather than by scanning a run: every entry in the key's
     * bucket whose fingerprint bits agree with the key's, over the length that
     * entry actually stores, less the lazy decrement once. The matching is
     * spelled out here from the stored length, so it does not lean on the same
     * bit tricks the query does.
     */
    template <bool E>
    static uint64_t ExpectedQuery(const SublimeMG<E>& mg, uint64_t key, uint8_t flags = 0) {
        const FingerprintTable& table = mg.table_;
        const uint64_t hash = table.hash_key(key, flags);
        const uint64_t bucket = table.bucket_from_hash(hash);
        const uint64_t target = table.fingerprint_from_hash(hash, table.GetNumFingerprintBits());

        uint64_t sum = 0, matches = 0;
        for (auto it = table.begin(); it != table.end(); ++it) {
            if (it.bucket() != bucket)
                continue;
            const uint64_t stored = it.fingerprint();
            uint32_t length = 0;
            while ((stored >> (length + 1)) != 0)
                length++;
            const uint64_t mask = (1ULL << length) - 1;
            if ((stored & mask) == (target & mask)) {
                sum += mg.Counters().Get(it.slot());
                matches++;
            }
        }
        // The decrement is the key's, once over, not each entry's.
        return matches == 0 ? 0 : sum - mg.lazy_decrement_;
    }

    /**
     * A query adds up every match in the key's run, of whatever length, and
     * leaves the entries that merely share the bucket out of it.
     */
    static void QuerySumsEveryMatch() {
        SublimeMG<> mg(256, 20, hashmode::Default, 3);
        FingerprintTable& table = mg.table_;
        const uint32_t full_length = table.GetNumFingerprintBits();
        REQUIRE_GE(full_length, 4);

        const uint64_t key = 0xBEEF;
        REQUIRE_EQ(mg.Query(key), 0);           // Nothing stored yet.

        // One matching fingerprint of every length the table can hold, planted
        // directly, so that the run carries the whole spread at once.
        const uint64_t hash = table.hash_key(key, 0);
        for (uint32_t length = 0; length <= full_length; length++)
            REQUIRE_GE(table.insert_hash_at(hash, length), 0);
        // And entries sharing the bucket without matching: the bucket comes
        // out of the low bits of the hash and the fingerprint out of the ones
        // just above them, so flipping the lowest fingerprint bit keeps the
        // bucket and spoils the match for every length but zero.
        const uint64_t other = hash ^ (1ULL << table.GetBucketIndexHashSize());
        REQUIRE_EQ(table.bucket_from_hash(other), table.bucket_from_hash(hash));
        for (uint32_t length = 1; length <= full_length; length++)
            REQUIRE_GE(table.insert_hash_at(other, length), 0);

        // Give every entry a distinct count, matching or not.
        uint64_t next = 1;
        for (auto it = table.begin(); it != table.end(); ++it)
            table.GetCounters()->Set(it.slot(), next++);
        REQUIRE_EQ(table.CountFingerprints(), 2 * full_length + 1);

        // Only the matches are summed -- one per length, so `full_length + 1`
        // of the `2 * full_length + 1` entries in the bucket.
        const uint64_t expected = ExpectedQuery(mg, key);
        REQUIRE_GT(expected, 0);
        REQUIRE_EQ(mg.Query(key), expected);
        REQUIRE_EQ(MatchCount(mg, key), full_length + 1);
        // The sum really is a sum: it stands above every one of its own terms.
        for (auto it = table.begin(); it != table.end(); ++it)
            REQUIRE_GT(mg.Query(key), table.GetCounters()->Get(it.slot()));

        // The lazy decrement is owed by the key, once, not by each of the
        // counters its count is spread over. So it comes off the sum once:
        // raising just the first entry of the chain by it changes nothing,
        // while raising every entry by it raises the answer.
        const uint64_t owed = 5;
        const uint64_t root = table.run_start(table.bucket_from_hash(hash));
        table.GetCounters()->Set(root, table.GetCounters()->Get(root) + owed);
        mg.lazy_decrement_ = owed;
        REQUIRE_EQ(mg.Query(key), expected);
        for (auto it = table.begin(); it != table.end(); ++it)
            if (it.slot() != root)
                table.GetCounters()->Set(it.slot(), table.GetCounters()->Get(it.slot()) + owed);
        REQUIRE_EQ(mg.Query(key), expected + full_length * owed);

        // A key whose bucket holds nothing at all.
        uint64_t elsewhere = 0;
        while (table.is_occupied(table.bucket_from_hash(table.hash_key(elsewhere, 0))))
            elsewhere++;
        REQUIRE_EQ(mg.Query(elsewhere), 0);
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
        SublimeMG<> mg(1024, 34, hashmode::Default, 45, 1, 32);
        std::mt19937_64 rng(46);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 100000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(20000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
        }
        mg.FlushBuffer();
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

    /**
     * A summary that fills up and expands, over and over, ends up holding
     * fingerprints of every length at once: an expansion takes a bit off the
     * entries already stored, while the ones admitted afterwards come in at
     * full length. The short ones stand for whole families of keys, so a run
     * holds several matches for one key, and adding all of them up is what
     * keeps the answer an over-estimate. Entries that have run out of
     * fingerprint entirely match everything, and are duplicated into both of
     * their candidate buckets besides -- but the two copies land in different
     * buckets, so a query still sees only one of them.
     */
    static void QuerySumsMatchesOfEveryLength() {
        SublimeMG<> mg(64, 12, hashmode::Default, 49);
        REQUIRE_EQ(mg.Table().GetNumFingerprintBits(), 6);

        // Six rounds of filling to capacity and expanding, which is one round
        // for every bit of fingerprint the oldest entries have to give up.
        std::mt19937_64 rng(50);
        std::vector<uint64_t> keys;
        for (uint32_t e = 0; e <= 6; e++) {
            while (mg.CountMonitored() < mg.Capacity()) {
                const uint64_t key = rng();
                REQUIRE_EQ(mg.StartMonitoring(key), 0);
                keys.push_back(key);
                // Its own entry is the longest match right now, so these land
                // on it rather than on anything else sharing the bucket.
                for (uint32_t k = 0; k < 3; k++)
                    REQUIRE_EQ(mg.Insert(key), 0);
            }
            if (e < 6)
                REQUIRE_GE(mg.Expand(), 0);
            CheckSummary(mg);
        }

        // Every length from spent to full is in there.
        std::set<uint32_t> lengths;
        for (auto it = mg.Table().begin(); it != mg.Table().end(); ++it)
            lengths.insert(it.fingerprint_length());
        REQUIRE_EQ(lengths, std::set<uint32_t>({0, 1, 2, 3, 4, 5, 6}));

        uint64_t summed_more_than_one = 0;
        for (const uint64_t key : keys) {
            const uint64_t query = mg.Query(key);
            REQUIRE_EQ(query, ExpectedQuery(mg, key));
            REQUIRE_GE(query, 4);       // Its own count, at the very least.
            summed_more_than_one += MatchCount(mg, key) > 1;
        }
        REQUIRE_GT(summed_more_than_one, 0);

        // And it answers for keys it never saw, by whatever their bucket holds.
        for (uint64_t key = 0; key < 64; key++)
            REQUIRE_EQ(mg.Query(key), ExpectedQuery(mg, key));
    }

    /**
     * @returns A key whose bucket holds nothing at all, so that a chain can be
     * planted in it without anything else of the table's running through it.
     */
    template <bool E>
    static uint64_t KeyInAnEmptyBucket(const SublimeMG<E>& mg, uint64_t from = 0) {
        const FingerprintTable& table = mg.table_;
        while (table.is_occupied(table.bucket_from_hash(table.hash_key(from, 0))))
            from++;
        return from;
    }

    /**
     * @returns A key sharing `like`'s bucket whose full-length fingerprint is
     * none of `apart_from`. Found by search, which is cheap: a bucket is one
     * of a few hundred here.
     */
    template <bool E>
    static uint64_t KeyInTheSameBucket(const SublimeMG<E>& mg, uint64_t like,
                                       const std::vector<uint64_t>& apart_from) {
        const FingerprintTable& table = mg.table_;
        const uint64_t bucket = table.bucket_from_hash(table.hash_key(like, 0));
        const uint64_t length = table.GetNumFingerprintBits();
        for (uint64_t key = like + 1;; key++) {
            const uint64_t hash = table.hash_key(key, 0);
            if (table.bucket_from_hash(hash) != bucket)
                continue;
            const uint64_t fingerprint = table.fingerprint_from_hash(hash, length);
            if (std::find(apart_from.begin(), apart_from.end(), fingerprint) == apart_from.end())
                return key;
        }
    }

    /**
     * Planted directly: a void entry holding `root_count`, and two
     * full-length extensions of it holding `a_count` and `b_count`. Chains do
     * not arise until the table has expanded, and building one by hand is a
     * good deal clearer than expanding until one turns up.
     *
     * @returns The key of the first extension, the key of the second, and a
     * third key of the same bucket that only the void entry matches.
     */
    template <bool E>
    static std::array<uint64_t, 3> PlantAChain(SublimeMG<E>& mg, uint64_t root_count,
                                               uint64_t a_count, uint64_t b_count) {
        FingerprintTable& table = mg.table_;
        const uint64_t length = table.GetNumFingerprintBits();

        const uint64_t a = KeyInAnEmptyBucket(mg);
        const uint64_t hash_a = table.hash_key(a, 0);
        const uint64_t fingerprint_a = table.fingerprint_from_hash(hash_a, length);
        const uint64_t b = KeyInTheSameBucket(mg, a, {fingerprint_a});
        const uint64_t hash_b = table.hash_key(b, 0);
        const uint64_t fingerprint_b = table.fingerprint_from_hash(hash_b, length);
        const uint64_t c = KeyInTheSameBucket(mg, a, {fingerprint_a, fingerprint_b});

        // The void entry first, so that it is the bottom of both chains.
        const int64_t root = table.insert_hash_at(hash_a, 0);
        REQUIRE_GE(root, 0);
        table.GetCounters()->Set(root, root_count);
        const int64_t at_a = table.insert_hash_at(hash_a, length);
        REQUIRE_GE(at_a, 0);
        table.GetCounters()->Set(at_a, a_count);
        const int64_t at_b = table.insert_hash_at(hash_b, length);
        REQUIRE_GE(at_b, 0);
        table.GetCounters()->Set(at_b, b_count);

        mg.recount();
        return {a, b, c};
    }

    /**
     * Removing an entry takes its counter out of the chain sums of the longer
     * entries that matched it. The repair hands it to the entries left at the
     * bottom of those chains, so every key's count comes through untouched --
     * and a key that the removed entry was the only match for reads zero,
     * which is what removing it was supposed to mean.
     */
    static void ChainSumsSurviveEviction() {
        SublimeMG<> mg(128, 26, hashmode::Default, 63);
        FingerprintTable& table = mg.table_;
        const auto [a, b, c] = PlantAChain(mg, 7, 11, 13);

        // Chain sums 7, 18 and 20: the void entry's own, and one for each
        // extension reaching down through it.
        REQUIRE_EQ(mg.Query(a), 18);
        REQUIRE_EQ(mg.Query(b), 20);
        REQUIRE_EQ(mg.Query(c), 7);
        CheckSummary(mg);

        const uint64_t bucket = table.bucket_from_hash(table.hash_key(a, 0));
        std::vector<std::pair<uint64_t, uint64_t>> victims{{bucket, table.run_start(bucket)}};
        const int64_t delta = table.DeleteEntriesPreservingChainSums(victims);
        mg.recount();

        // Seven came out of one entry and went into each of the two above it:
        // the only case where a removal leaves the counters holding more than
        // they did, and the price of two chains sharing one entry.
        REQUIRE_EQ(delta, 7);
        REQUIRE_EQ(mg.CountMonitored(), 2);
        REQUIRE_EQ(mg.Query(a), 18);
        REQUIRE_EQ(mg.Query(b), 20);
        REQUIRE_EQ(mg.Query(c), 0);     // Nothing matches it any more.
        CheckSummary(mg);
    }

    /**
     * The repair hands what was removed to the entry left at the *bottom* of
     * each chain, and to that entry only. Every entry above it reaches the
     * removed counters through it, so its sum is repaired along with theirs --
     * and giving them a share of their own would count the same counter twice.
     *
     * That only shows up in a chain at least three deep, where an entry can
     * lose its prefix and still have one left underneath.
     */
    static void RepairStopsAtTheFirstSurvivor() {
        SublimeMG<> mg(128, 26, hashmode::Default, 66);
        FingerprintTable& table = mg.table_;
        const uint64_t length = table.GetNumFingerprintBits();
        REQUIRE_GT(length, 3);

        // Void, three bits, and full length -- each one a prefix of the next,
        // since all three are fingerprints of the same hash.
        const uint64_t key = KeyInAnEmptyBucket(mg);
        const uint64_t hash = table.hash_key(key, 0);
        for (const auto& [len, count] : std::vector<std::pair<uint64_t, uint64_t>>{
                    {0, 4}, {3, 6}, {length, 10}}) {
            const int64_t at = table.insert_hash_at(hash, len);
            REQUIRE_GE(at, 0);
            table.GetCounters()->Set(at, count);
        }
        mg.recount();
        REQUIRE_EQ(mg.Query(key), 20);      // 4 + 6 + 10.
        CheckSummary(mg);

        const uint64_t bucket = table.bucket_from_hash(hash);
        std::vector<std::pair<uint64_t, uint64_t>> victims{{bucket, table.run_start(bucket)}};
        const int64_t delta = table.DeleteEntriesPreservingChainSums(victims);
        mg.recount();

        // The four went to the three-bit entry and stopped there: the counters
        // still add up to what they did, and the key still reads twenty.
        REQUIRE_EQ(delta, 0);
        REQUIRE_EQ(mg.Query(key), 20);
        const uint64_t at = table.run_start(bucket);
        REQUIRE_EQ(mg.Counters().Get(at), 10);
        REQUIRE_EQ(mg.Counters().Get(at + 1), 10);
        CheckSummary(mg);
    }

    /**
     * Eviction is driven by chain sums, so a short fingerprint goes before the
     * longer ones extending it: its chain sum is the smaller of the two by
     * construction, whatever the two counters say. The extension keeps its
     * count exactly, having been handed what was removed from under it.
     */
    static void EvictionTakesThePrefixAndKeepsTheExtension() {
        SublimeMG<> mg(128, 26, hashmode::Default, 61, 1, /*buffer_capacity=*/1);
        FingerprintTable& table = mg.table_;

        // A summary of well-counted keys, so that nothing else is anywhere
        // near being evicted, and then the chain, two slots from full.
        const auto keys = DistinctKeys(mg, mg.Capacity() - 3, 62);
        for (const uint64_t key : keys) {
            REQUIRE_EQ(mg.StartMonitoring(key), 0);
            for (uint32_t k = 1; k < 1000; k++)
                REQUIRE_EQ(mg.Insert(key), 0);
        }
        const auto [a, b, c] = PlantAChain(mg, 3, 500, 800);
        REQUIRE_EQ(mg.CountMonitored(), mg.Capacity());
        REQUIRE_EQ(mg.Query(a), 503);
        REQUIRE_EQ(mg.Query(b), 803);
        REQUIRE_EQ(mg.Query(c), 3);

        // Three occurrences of keys it has never seen, one decrement each --
        // the buffer holds one at a time here. The void entry's chain sum of
        // three is the smallest in the table, so the third decrement is the
        // one that catches up with it.
        for (uint32_t i = 0; i < 3; i++)
            REQUIRE_EQ(mg.Insert(KeyInAnEmptyBucket(mg, 1000 + i)), 0);
        REQUIRE_EQ(mg.CountDecrements(), 3);
        REQUIRE_EQ(mg.GetLazyDecrement(), 3);
        CheckSummary(mg);

        // The void entry went, both extensions stayed, and their keys are down
        // by exactly the three decrements -- not by the counter that came out
        // from under them, which each of them was handed instead.
        REQUIRE_EQ(mg.Query(c), 0);
        REQUIRE_EQ(mg.Query(a), 500);
        REQUIRE_EQ(mg.Query(b), 800);
        const uint64_t bucket = table.bucket_from_hash(table.hash_key(a, 0));
        const uint64_t at = table.run_start(bucket);
        REQUIRE_EQ(mg.Counters().Get(at) + mg.Counters().Get(at + 1), 503 + 803);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), 997);
    }

    /**
     * What decides whether an entry can free a slot is its chain sum, not its
     * own counter. An entry sitting on a well-counted prefix holds very little
     * itself and is nowhere near being evicted, because the key family ending
     * there is as large as the whole chain says. Judging it by its own counter
     * would free a slot that had not emptied, and let the batch admit a key
     * over capacity against it.
     */
    static void ChainedEntriesAreJudgedByTheirChainSum() {
        SublimeMG<> mg(128, 26, hashmode::Default, 67, 1, /*buffer_capacity=*/1);

        const auto keys = DistinctKeys(mg, mg.Capacity() - 3, 68);
        for (const uint64_t key : keys) {
            REQUIRE_EQ(mg.StartMonitoring(key), 0);
            for (uint32_t k = 1; k < 1000; k++)
                REQUIRE_EQ(mg.Insert(key), 0);
        }
        // A thousand at the bottom of the chain, one in each entry above it.
        const auto [a, b, c] = PlantAChain(mg, 1000, 1, 1);
        REQUIRE_EQ(mg.CountMonitored(), mg.Capacity());
        REQUIRE_EQ(mg.Query(a), 1001);
        REQUIRE_EQ(mg.Query(b), 1001);
        REQUIRE_EQ(mg.Query(c), 1000);
        CheckSummary(mg);

        // One occurrence of a key it has never seen. Nothing in the table is
        // within a thousand of empty, so the decrement frees nothing and the
        // occurrence is lost -- the entries holding one apiece included.
        const uint64_t unseen = KeyInAnEmptyBucket(mg, 2000);
        REQUIRE_EQ(mg.Insert(unseen), 0);
        REQUIRE_EQ(mg.CountDecrements(), 1);
        REQUIRE_EQ(mg.CountMonitored(), mg.Capacity());
        REQUIRE_EQ(mg.Query(unseen), 0);
        REQUIRE_EQ(mg.Query(a), 1000);
        REQUIRE_EQ(mg.Query(b), 1000);
        REQUIRE_EQ(mg.Query(c), 999);
        for (const uint64_t key : keys)
            REQUIRE_EQ(mg.Query(key), 999);
        CheckSummary(mg);
    }

    /**
     * A chain carries the lazy decrement in one place, its first entry, so
     * that is the one place merging takes it off. Every count reads exactly
     * what it read before.
     */
    static void MergeSubtractsOncePerChain() {
        SublimeMG<> mg(128, 26, hashmode::Default, 64);
        FingerprintTable& table = mg.table_;
        const auto [a, b, c] = PlantAChain(mg, 7, 11, 13);

        mg.lazy_decrement_ = 5;
        REQUIRE_EQ(mg.Query(a), 13);
        REQUIRE_EQ(mg.Query(b), 15);
        REQUIRE_EQ(mg.Query(c), 2);
        CheckSummary(mg);

        mg.MergeLazyDecrement();
        REQUIRE_EQ(mg.GetLazyDecrement(), 0);
        REQUIRE_EQ(mg.Query(a), 13);
        REQUIRE_EQ(mg.Query(b), 15);
        REQUIRE_EQ(mg.Query(c), 2);
        CheckSummary(mg);

        // It came off the void entry alone. Taking it off all three would have
        // cut both extensions' keys by another five apiece.
        const uint64_t bucket = table.bucket_from_hash(table.hash_key(a, 0));
        uint64_t pos = table.run_start(bucket);
        REQUIRE_EQ(mg.Counters().Get(pos), 2);
        REQUIRE_EQ(mg.Counters().Get(pos + 1) + mg.Counters().Get(pos + 2), 11 + 13);

        // And merging again changes nothing.
        mg.MergeLazyDecrement();
        REQUIRE_EQ(mg.Query(a), 13);
        REQUIRE_EQ(mg.Query(b), 15);
    }

    /**
     * The lazy decrement is carried by the first entry of a chain, so a key
     * admitted onto a chain that already has one must not carry it again: its
     * entry holds just its own count. Paying it twice would credit the new key
     * with the whole decrement on top of the chain it landed on.
     */
    static void AdmittingOntoAChainDoesNotPayTheDecrementTwice() {
        SublimeMG<> mg(128, 26, hashmode::Default, 69);
        const auto [a, b, c] = PlantAChain(mg, 7, 11, 13);
        mg.lazy_decrement_ = 5;
        REQUIRE_EQ(mg.Query(c), 2);
        CheckSummary(mg);

        // `c` matches the void entry at the bottom of the chain, so admitting
        // it counts it up by one from what it already read.
        REQUIRE_EQ(mg.StartMonitoring(c), 0);
        REQUIRE_EQ(mg.Query(c), 3);
        REQUIRE_EQ(mg.Query(a), 13);        // The others are untouched.
        REQUIRE_EQ(mg.Query(b), 15);
        CheckSummary(mg);

        // A key with nothing matching it starts a chain of its own, and that
        // entry does have to carry the decrement.
        const uint64_t fresh = KeyInAnEmptyBucket(mg, 5000);
        REQUIRE_EQ(mg.StartMonitoring(fresh), 0);
        REQUIRE_EQ(mg.Query(fresh), 1);
        REQUIRE_EQ(mg.Counters().Get(mg.table_.FindLongestMatch(fresh)), 6);
        CheckSummary(mg);
    }

    /**
     * Two keys of one batch that the table cannot tell apart are one key as
     * far as the summary goes: they share a slot, and the second counts the
     * first up rather than spending a second one.
     */
    static void BatchMergesKeysItCannotTellApart() {
        SublimeMG<> mg(128, 26, hashmode::Default, 65, 1, /*buffer_capacity=*/4);
        const uint64_t key_bits = mg.Table().GetNumKeyBits();

        // Two hashes agreeing on every bit the table keeps, and differing
        // above them, so they are distinct keys with one entry between them.
        const uint8_t flags = SublimeMG<>::flag_key_is_hash;
        const uint64_t first = 0x123456;
        const uint64_t second = first | (1ULL << key_bits);
        REQUIRE_NE(first, second);
        REQUIRE_EQ(mg.Table().EntryIdentity(first, flags),
                   mg.Table().EntryIdentity(second, flags));

        mg.buffer_.push_back({first, flags});
        mg.buffer_.push_back({second, flags});
        REQUIRE_EQ(mg.FlushBuffer(), 0);

        REQUIRE_EQ(mg.CountMonitored(), 1);
        REQUIRE_EQ(mg.CountDecrements(), 0);
        REQUIRE_EQ(mg.Query(first, flags), 2);
        REQUIRE_EQ(mg.Query(second, flags), 2);
        CheckSummary(mg);
    }

    /** Stretching changes the table's shape, and the counters follow it. */
    static void WorksUnderStretching() {
        SublimeMG<> mg(300, 26, hashmode::Default, 1, /*growth_coefficient=*/3);
        REQUIRE_EQ(mg.Table().GetGrowthCoefficient(), 3);
        REQUIRE_EQ(mg.Counters().CountCounters(), mg.Table().GetSlotCapacity());

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
        SublimeMG<false> mg(256, 30, hashmode::Default, 71, 1, 32, f);

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

        mg.FlushBuffer();
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
        SublimeMG<> mg(256, 30, hashmode::Default, 73, 1, 32);
        REQUIRE_EQ(mg.GetExpansionLimit(), std::numeric_limits<uint64_t>::max());

        const uint64_t capacity = mg.Capacity();
        for (uint64_t i = 0; i < 20000; i++)
            REQUIRE_EQ(mg.Insert(i % 5000), 0);
        mg.FlushBuffer();

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
        SublimeMG<false> mg(256, 30, hashmode::Default, 75, 1, 32, f);
        REQUIRE_EQ(mg.GetExpansionLimit(), 5000);

        for (uint64_t i = 0; i < 5000; i++)
            REQUIRE_EQ(mg.Insert(i % 64), 0);
        REQUIRE_EQ(mg.CountExpansions(), 0);

        // One insertion crosses every size the jump skipped over.
        REQUIRE_EQ(mg.Insert(7), 0);
        REQUIRE_GT(mg.CountExpansions(), 1);
        REQUIRE_GE(mg.Capacity(), 4000);
        REQUIRE_EQ(mg.GetExpansionLimit(), std::numeric_limits<uint64_t>::max());

        mg.FlushBuffer();
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
        SublimeMG<false> mg(64, 30, hashmode::Default, 83, 1, 8, f);
        REQUIRE_EQ(mg.GetExpansionLimit(), 0);
        const uint64_t capacity_before = mg.Capacity();

        for (uint64_t i = 0; i < 2000; i++) {
            REQUIRE_EQ(mg.Insert(i % 32), 0);
            // It expands only while it holds fewer slots than the stream has
            // elements, and an expansion at most doubles it, so it stays under
            // twice the stream however loudly the size function asks.
            REQUIRE_LE(mg.Capacity(), std::max(capacity_before, 2 * (i + 1)));
        }
        mg.FlushBuffer();
        REQUIRE_GT(mg.CountExpansions(), 0);
        REQUIRE_LT(mg.Capacity(), 4000);
        CheckSummary(mg);
    }

    /**
     * A table with no hash bits left to spend cannot expand however loudly the
     * size function asks, so the threshold is retired rather than tested once
     * per insertion for the rest of the stream.
     */
    static void StopsTestingWhenItCannotGrow() {
        auto f = [](double) { return uint64_t{1}; };
        // 63 key bits over 256 slots is a 55-bit fingerprint, the widest slot
        // the table allows; expanding would take a 65th bit of the hash, and
        // there is none.
        SublimeMG<> mg(256, 63, hashmode::Default, 77, 1, 8, f);
        REQUIRE_EQ(mg.Table().GetNumFingerprintBits(), 55);
        REQUIRE_EQ(mg.Table().CountSlotsAfterExpansion(), mg.Table().CountSlots());
        REQUIRE_EQ(mg.GetExpansionLimit(), std::numeric_limits<uint64_t>::max());

        for (uint64_t i = 0; i < 500; i++)
            REQUIRE_EQ(mg.Insert(i % 200), 0);
        mg.FlushBuffer();
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
        SublimeMG<> mg(128, 34, hashmode::Default, 79, /*growth_coefficient=*/2, 32, f);
        const uint64_t capacity_before = mg.Capacity();

        std::mt19937_64 rng(79);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 300000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(60000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(mg.Insert(key), 0);
        }
        mg.FlushBuffer();
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
        SublimeMG<> mg(256, 30, hashmode::Default, 85, 1, 32);
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
        SublimeMG<> mg(256, 30, hashmode::Default, 87, 1, 32, f);
        REQUIRE_EQ(mg.GetExpansionLimit(), 10);

        const auto keys = DistinctKeys(mg, 50, 87);
        for (uint64_t round = 0; round < 400; round++)
            for (const uint64_t key : keys)
                REQUIRE_EQ(mg.Insert(key), 0);
        mg.FlushBuffer();

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
        SublimeMG<> selective(256, 34, hashmode::Default, 89, 1, 32, f);
        SublimeMG<false> everything(256, 34, hashmode::Default, 89, 1, 32, f);

        std::mt19937_64 rng(89);
        std::vector<uint64_t> stream;
        for (int32_t i = 0; i < 200000; i++) {
            const double u = (rng() % 1000000) / 1000000.0;
            const uint64_t key = static_cast<uint64_t>(40000 * u * u * u);
            stream.push_back(key);
            REQUIRE_EQ(selective.Insert(key), 0);
            REQUIRE_EQ(everything.Insert(key), 0);
        }
        selective.FlushBuffer();
        everything.FlushBuffer();
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
        SublimeMG<false> mg(256, 30, hashmode::Default, 81, 1, 32, f);
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
    TEST_CASE("insert increments the longest match") {
        SublimeMGTest::InsertIncrementsTheLongestMatch();
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

    TEST_CASE("void entries keep the whole count twice") {
        SublimeMGTest::VoidEntriesKeepTheWholeCountTwice();
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

    TEST_CASE("query sums every match") {
        SublimeMGTest::QuerySumsEveryMatch();
    }

    TEST_CASE("query agrees with count when matches are unique") {
        SublimeMGTest::QueryAgreesWithCountWhenMatchesAreUnique();
    }

    TEST_CASE("query sums matches of every length") {
        SublimeMGTest::QuerySumsMatchesOfEveryLength();
    }

    TEST_CASE("chain sums survive eviction") {
        SublimeMGTest::ChainSumsSurviveEviction();
    }

    TEST_CASE("repair stops at the first survivor") {
        SublimeMGTest::RepairStopsAtTheFirstSurvivor();
    }

    TEST_CASE("eviction takes the prefix and keeps the extension") {
        SublimeMGTest::EvictionTakesThePrefixAndKeepsTheExtension();
    }

    TEST_CASE("chained entries are judged by their chain sum") {
        SublimeMGTest::ChainedEntriesAreJudgedByTheirChainSum();
    }

    TEST_CASE("merge subtracts once per chain") {
        SublimeMGTest::MergeSubtractsOncePerChain();
    }

    TEST_CASE("admitting onto a chain does not pay the decrement twice") {
        SublimeMGTest::AdmittingOntoAChainDoesNotPayTheDecrementTwice();
    }

    TEST_CASE("batch merges keys it cannot tell apart") {
        SublimeMGTest::BatchMergesKeysItCannotTellApart();
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

    TEST_CASE("buffers when full") {
        SublimeMGTest::BuffersWhenFull();
    }

    TEST_CASE("evicts the smallest counters") {
        SublimeMGTest::EvictsTheSmallestCounters();
    }

    TEST_CASE("many tied counters empty at once") {
        SublimeMGTest::ManyTiedCountersEmptyAtOnce();
    }

    TEST_CASE("lazy decrement and merging") {
        SublimeMGTest::LazyDecrementAndMerging();
    }

    TEST_CASE("merges on its own") {
        SublimeMGTest::MergesOnItsOwn();
    }

    TEST_CASE("retuning applies the lazy decrement") {
        SublimeMGTest::RetuningAppliesTheLazyDecrement();
    }

    TEST_CASE("keeps the misra-gries guarantee") {
        SublimeMGTest::KeepsTheMisraGriesGuarantee();
    }

    TEST_CASE("survives a stream with no heavy hitters") {
        SublimeMGTest::SurvivesAStreamWithNoHeavyHitters();
    }

    TEST_CASE("keeps the guarantee across resizes") {
        SublimeMGTest::KeepsTheGuaranteeAcrossResizes();
    }
}
