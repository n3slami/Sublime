#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>
#include <assert.h>

#include "FingerprintTable.hpp"

namespace sublime {

/**
 * White-box tests for `FingerprintTable`. Declared a `friend` of the table, so
 * these reach into the slots and the rank/select metadata directly.
 */
class FingerprintTableTest {
public:
    using hashmode = FingerprintTable::hashmode;

    /**
     * Checks everything the rank-and-select encoding is supposed to guarantee:
     * the runends and occupieds bitmaps agree, runs live at or after their
     * canonical slot, do not overlap, and are sorted by slot value.
     */
    static void CheckStructure(const FingerprintTable& t) {
        uint64_t occupied_buckets = 0, runend_count = 0, slots_in_runs = 0;
        for (uint64_t i = 0; i < t.xnslots_; i++) {
            occupied_buckets += t.is_occupied(i);
            runend_count += t.is_runend(i);
        }
        REQUIRE_EQ(occupied_buckets, runend_count);

        int64_t previous_end = -1;
        std::vector<bool> in_run(t.xnslots_, false);
        for (uint64_t b = 0; b < t.xnslots_; b++) {
            if (!t.is_occupied(b))
                continue;
            const uint64_t start = t.run_start(b);
            const uint64_t end = t.run_end(b);
            // A run never starts before its canonical slot, and runs appear in
            // the order of the buckets they belong to.
            REQUIRE_GE(start, b);
            REQUIRE_GE(end, start);
            REQUIRE_GT(static_cast<int64_t>(start), previous_end);
            REQUIRE(t.is_runend(end));

            uint64_t previous_slot = 0;
            for (uint64_t i = start; i <= end; i++) {
                const uint64_t slot = t.get_slot(i);
                // Runs are kept sorted. Note a stored fingerprint may well be
                // zero -- there is no void bit to rule that out any more, so
                // what is stored says nothing about whether a slot is in use.
                REQUIRE_GE(slot, previous_slot);
                REQUIRE_LT(slot, 1ULL << t.GetNumFingerprintBits());
                REQUIRE_EQ(t.is_runend(i), i == end);
                previous_slot = slot;
                in_run[i] = true;
                slots_in_runs++;
            }
            previous_end = end;
        }
        REQUIRE_EQ(slots_in_runs, t.noccupied_slots_);

        // Nothing outside a run may hold anything: a slot a shift or a
        // deletion vacated is left cleared, not merely unreachable.
        for (uint64_t i = 0; i < t.xnslots_; i++)
            if (!in_run[i])
                REQUIRE_EQ(t.get_slot(i), 0);
    }

    /** @returns The multiset of (bucket, raw slot) pairs the table holds. */
    static std::multiset<std::pair<uint64_t, uint64_t>> Contents(const FingerprintTable& t) {
        std::multiset<std::pair<uint64_t, uint64_t>> res;
        for (auto it = t.begin(); it != t.end(); ++it)
            res.emplace(it.bucket(), it.fingerprint());
        return res;
    }

    static void SlotReadWrite() {
        // A slot width that divides neither 8 nor 64, so slots straddle byte
        // and word boundaries.
        FingerprintTable t(1024, 24, hashmode::Default, 1);
        REQUIRE_EQ(t.GetNumFingerprintBits(), 14);
        REQUIRE_EQ(t.GetBitsPerSlot(), 14);

        std::mt19937_64 rng(1);
        std::vector<uint64_t> ref(t.xnslots_);
        const uint64_t mask = fpt::bitmask(t.GetBitsPerSlot());
        for (uint64_t i = 0; i < t.xnslots_; i++) {
            ref[i] = rng() & mask;
            t.set_slot(i, ref[i]);
        }
        for (uint64_t i = 0; i < t.xnslots_; i++)
            REQUIRE_EQ(t.get_slot(i), ref[i]);

        // Overwriting one slot must leave its neighbors alone.
        for (uint64_t i = 1; i + 1 < t.xnslots_; i += 37) {
            ref[i] = rng() & mask;
            t.set_slot(i, ref[i]);
            REQUIRE_EQ(t.get_slot(i - 1), ref[i - 1]);
            REQUIRE_EQ(t.get_slot(i), ref[i]);
            REQUIRE_EQ(t.get_slot(i + 1), ref[i + 1]);
        }
    }

    static void Hashing() {
        const uint64_t nslots = 1000;    // Not a power of two.
        FingerprintTable t(nslots, 22, hashmode::Default, 5);
        REQUIRE_EQ(t.GetOriginalQuotientBits(), 10);
        REQUIRE_EQ(t.GetBucketIndexHashSize(), 10);
        REQUIRE_EQ(t.GetNumFingerprintBits(), 12);
        REQUIRE_EQ(t.GetExpansionCount(), 0);

        std::mt19937_64 rng(2);
        for (int32_t i = 0; i < 10000; i++) {
            const uint64_t key = rng();
            const uint64_t hash = t.hash_key(key, 0);
            const uint64_t bucket = t.bucket_from_hash(hash);
            // The fast reduction of the original quotient keeps every bucket
            // inside a table whose size is not a power of two.
            REQUIRE_LT(bucket, nslots);
            const uint64_t fingerprint = t.fingerprint_from_hash(hash);
            // A fingerprint is exactly the hash bits above the bucket index,
            // and nothing wider: bucket and fingerprint partition the hash.
            REQUIRE_LT(fingerprint, 1ULL << t.GetNumFingerprintBits());
            REQUIRE_EQ(fingerprint, (hash >> t.GetBucketIndexHashSize())
                                            & fpt::bitmask(t.GetNumFingerprintBits()));
        }
    }

    static void InsertAndCount() {
        FingerprintTable t(1024, 24, hashmode::Default, 1);
        std::mt19937_64 rng(3);
        std::map<uint64_t, uint64_t> ref;

        const int32_t n = 700;
        for (int32_t i = 0; i < n; i++) {
            const uint64_t key = rng() % 100000;
            REQUIRE_EQ(t.Insert(key), 0);
            ref[key]++;
        }
        CheckStructure(t);
        REQUIRE_EQ(t.CountFingerprints(), n);
        REQUIRE_EQ(t.CountOccupiedSlots(), n);
        REQUIRE_EQ(t.LoadFactor(), doctest::Approx(static_cast<double>(n) / t.CountSlots()));

        for (const auto& [key, count] : ref) {
            // The table is a multiset that never undercounts.
            REQUIRE_GE(t.Count(key), count);
            REQUIRE(t.Contains(key));
            // The match holds exactly the key's fingerprint.
            const int64_t at = t.FindMatch(key);
            REQUIRE_GE(at, 0);
            REQUIRE_EQ(t.get_slot(at), t.fingerprint_from_hash(t.hash_key(key, 0)));
        }

        // The iterator sees exactly the stored fingerprints, and the hash it
        // reconstructs lands back in the same place.
        uint64_t seen = 0;
        for (auto it = t.begin(); it != t.end(); ++it) {
            const uint64_t hash = it.hash();
            REQUIRE_EQ(t.bucket_from_hash(hash), it.bucket());
            REQUIRE_EQ(t.fingerprint_from_hash(hash), it.fingerprint());
            seen++;
        }
        REQUIRE_EQ(seen, t.CountFingerprints());
    }

    static void DeleteRemovesMatch() {
        FingerprintTable t(256, 20, hashmode::Default, 1);
        const uint64_t key = 0xABCDEF;
        const uint64_t hash = t.hash_key(key, 0);
        const uint64_t bucket = t.bucket_from_hash(hash);

        // The table is a multiset: the same fingerprint can be stored more
        // than once, and each deletion peels one copy off.
        for (int32_t i = 0; i < 5; i++)
            REQUIRE_EQ(t.insert_hash(hash), 0);
        CheckStructure(t);
        REQUIRE_EQ(t.CountFingerprints(), 5);
        REQUIRE_EQ(t.Count(key), 5);

        for (int32_t left = 4; left >= 0; left--) {
            REQUIRE_EQ(t.Delete(key), 0);
            CheckStructure(t);
            REQUIRE_EQ(t.Count(key), static_cast<uint64_t>(left));
        }
        REQUIRE_EQ(t.CountFingerprints(), 0);
        REQUIRE_FALSE(t.Contains(key));
        REQUIRE_EQ(t.FindMatch(key), -1);
        REQUIRE_EQ(t.Delete(key), FingerprintTable::err_doesnt_exist);

        // A fingerprint that disagrees with the key is not a match, even in
        // the key's own run: every fingerprint is of the same length, so
        // matching is equality and nothing is a weaker witness than anything
        // else.
        const uint64_t other_hash = hash ^ (1ULL << t.GetBucketIndexHashSize());
        REQUIRE_EQ(t.insert_hash(other_hash), 0);
        REQUIRE_EQ(t.bucket_from_hash(other_hash), bucket);
        REQUIRE_FALSE(t.Contains(key));
        REQUIRE_EQ(t.Delete(key), FingerprintTable::err_doesnt_exist);
        REQUIRE_EQ(t.CountFingerprints(), 1);
    }

    /** Inserts and deletes at random, checking the structure as it goes. */
    static void InsertDeleteMonteCarlo() {
        FingerprintTable t(512, 22, hashmode::Default, 7);
        std::mt19937_64 rng(4);
        std::map<uint64_t, uint64_t> ref;
        std::vector<uint64_t> live;

        for (int32_t step = 0; step < 20000; step++) {
            const bool insert = live.empty() || (rng() % 3);
            if (insert) {
                if (t.CountFingerprints() + 1 >= t.CountSlots() * FingerprintTable::max_load_factor)
                    continue;
                const uint64_t key = rng() % 5000;
                REQUIRE_EQ(t.Insert(key), 0);
                ref[key]++;
                live.push_back(key);
            }
            else {
                const size_t index = rng() % live.size();
                const uint64_t key = live[index];
                live[index] = live.back();
                live.pop_back();
                REQUIRE_EQ(t.Delete(key), 0);
                if (--ref[key] == 0)
                    ref.erase(key);
            }
            if (step % 1000 == 0)
                CheckStructure(t);
        }
        CheckStructure(t);
        REQUIRE_EQ(t.CountFingerprints(), live.size());
        for (const auto& [key, count] : ref)
            REQUIRE_GE(t.Count(key), count);

        // Draining the table leaves it exactly as empty as a fresh one.
        for (const uint64_t key : live)
            REQUIRE_EQ(t.Delete(key), 0);
        CheckStructure(t);
        REQUIRE_EQ(t.CountFingerprints(), 0);
        REQUIRE_EQ(t.begin(), t.end());
    }

    /** Fills the table up to its maximum load factor. */
    static void HighLoadFactor() {
        FingerprintTable t(1024, 26, hashmode::Default, 9);
        std::mt19937_64 rng(5);
        std::vector<uint64_t> keys;
        while (t.CountFingerprints() < t.CountSlots() * FingerprintTable::max_load_factor) {
            const uint64_t key = rng();
            REQUIRE_EQ(t.Insert(key), 0);
            keys.push_back(key);
        }
        CheckStructure(t);
        REQUIRE_GT(t.LoadFactor(), 0.9);
        for (const uint64_t key : keys)
            REQUIRE(t.Contains(key));
        for (const uint64_t key : keys)
            REQUIRE_EQ(t.Delete(key), 0);
        CheckStructure(t);
        REQUIRE_EQ(t.CountFingerprints(), 0);
    }

    static void AutoExpand() {
        FingerprintTable t(128, 20, hashmode::Default, 11);
        REQUIRE_FALSE(t.IsAutoExpandEnabled());
        t.SetAutoExpand(true);
        REQUIRE(t.IsAutoExpandEnabled());

        std::mt19937_64 rng(6);
        std::vector<uint64_t> keys;
        for (int32_t i = 0; i < 4000; i++) {
            const uint64_t key = rng();
            REQUIRE_EQ(t.Insert(key), 0);
            keys.push_back(key);
        }
        CheckStructure(t);
        REQUIRE_GT(t.CountSlots(), 128);
        REQUIRE_LT(t.LoadFactor(), FingerprintTable::max_load_factor);
        REQUIRE_EQ(t.CountFingerprints(), keys.size());
        for (const uint64_t key : keys)
            REQUIRE(t.Contains(key));

        // Without auto-expansion, a full table refuses further insertions
        // instead of growing.
        FingerprintTable small(64, 16, hashmode::None, 1);
        int32_t ret = 0;
        for (uint64_t key = 0; ret == 0; key += 1ULL << small.GetBucketIndexHashSize())
            ret = small.Insert(key);   // Every key lands in bucket 0.
        REQUIRE_EQ(ret, FingerprintTable::err_no_space);
        CheckStructure(small);
    }

    /**
     * Every expansion shortens the stored fingerprints by one bit, and no key
     * is ever lost.
     */
    static void Expansion() {
        FingerprintTable t(256, 20, hashmode::Default, 3);
        std::mt19937_64 rng(7);
        std::map<uint64_t, uint64_t> ref;
        for (int32_t i = 0; i < 150; i++) {
            const uint64_t key = rng();
            REQUIRE_EQ(t.Insert(key), 0);
            ref[key]++;
        }
        const uint64_t full_length = t.GetNumFingerprintBits();
        const uint64_t original_slots = t.CountSlots();
        const uint64_t original_key_bits = t.GetNumKeyBits();

        for (uint32_t e = 1; e <= 5; e++) {
            REQUIRE_EQ(t.Expand(), 150);
            CheckStructure(t);
            REQUIRE_EQ(t.CountSlots(), original_slots << e);
            REQUIRE_EQ(t.GetExpansionCount(), e);
            // The bucket index took a bit off every fingerprint, stored and
            // fresh alike, so the slots got one bit narrower with it.
            REQUIRE_EQ(t.GetNumFingerprintBits(), full_length - e);
            REQUIRE_EQ(t.GetBitsPerSlot(), full_length - e);
            REQUIRE_EQ(t.GetNumKeyBits(), original_key_bits);
            REQUIRE_EQ(t.GetOriginalQuotientBits(), 8);

            for (const auto& [key, count] : ref) {
                REQUIRE_GE(t.Count(key), count);
                REQUIRE(t.Contains(key));
            }
            for (auto it = t.begin(); it != t.end(); ++it)
                REQUIRE_LT(it.bucket(), t.CountSlots());
        }
    }

    /**
     * A fingerprint cannot be shortened past its last bit, so a table that has
     * spent them all refuses to grow -- and says so beforehand, which is what
     * a caller driving expansion from a size function reads.
     */
    static void GrowthStopsWhenFingerprintsRunOut() {
        // A small table, so that fingerprints run out after a few expansions.
        FingerprintTable t(64, 14, hashmode::Default, 3);
        const uint64_t full_length = t.GetNumFingerprintBits();
        REQUIRE_EQ(full_length, 8);

        std::mt19937_64 rng(11);
        std::vector<uint64_t> keys;
        for (int32_t i = 0; i < 30; i++) {
            const uint64_t key = rng();
            REQUIRE_EQ(t.Insert(key), 0);
            keys.push_back(key);
        }

        // Every expansion sheds a bit, and the entry count never moves: with
        // one length there are no ambiguous entries to duplicate.
        for (uint32_t e = 1; e < full_length; e++) {
            REQUIRE_GT(t.CountSlotsAfterExpansion(), t.CountSlots());
            REQUIRE_EQ(t.Expand(), keys.size());
            CheckStructure(t);
            REQUIRE_EQ(t.GetNumFingerprintBits(), full_length - e);
            REQUIRE_EQ(t.CountFingerprints(), keys.size());
            for (const uint64_t key : keys)
                REQUIRE(t.Contains(key));
        }

        // One bit left: the table is done growing, and predicts as much.
        REQUIRE_EQ(t.GetNumFingerprintBits(), 1);
        const auto contents = Contents(t);
        const uint64_t slots = t.CountSlots();
        REQUIRE_EQ(t.CountSlotsAfterExpansion(), slots);
        REQUIRE_EQ(t.Expand(), FingerprintTable::err_no_space);
        // A refused expansion leaves the table exactly as it was.
        REQUIRE_EQ(t.CountSlots(), slots);
        REQUIRE(Contents(t) == contents);
        for (const uint64_t key : keys)
            REQUIRE(t.Contains(key));

        // And it contracts all the way back, exactly.
        while (t.GetExpansionCount() > 0)
            REQUIRE_EQ(t.Contract(), keys.size());
        REQUIRE_EQ(t.GetNumFingerprintBits(), full_length);
        REQUIRE_EQ(t.CountFingerprints(), keys.size());
        for (const uint64_t key : keys)
            REQUIRE(t.Contains(key));
    }

    /** Contraction is the exact inverse of expansion. */
    static void ContractionInvertsExpansion() {
        FingerprintTable t(256, 20, hashmode::Default, 13);
        std::mt19937_64 rng(9);
        std::map<uint64_t, uint64_t> ref;
        for (int32_t i = 0; i < 150; i++) {
            const uint64_t key = rng();
            REQUIRE_EQ(t.Insert(key), 0);
            ref[key]++;
        }
        const auto original = Contents(t);
        const uint64_t full_length = t.GetNumFingerprintBits();
        const uint64_t original_slots = t.CountSlots();
        const uint64_t original_key_bits = t.GetNumKeyBits();

        REQUIRE_EQ(t.Contract(), FingerprintTable::err_cannot_contract);
        REQUIRE_EQ(t.CountSlots(), original_slots);

        for (uint32_t e = 1; e <= 5; e++)
            REQUIRE_EQ(t.Expand(), 150);
        for (uint32_t e = 5; e > 0; e--) {
            REQUIRE_EQ(t.Contract(), 150);
            CheckStructure(t);
            REQUIRE_EQ(t.CountSlots(), original_slots << (e - 1));
            REQUIRE_EQ(t.GetExpansionCount(), e - 1);
            for (const auto& [key, count] : ref) {
                REQUIRE_GE(t.Count(key), count);
                REQUIRE(t.Contains(key));
            }
            // Contraction hands the bit back, exactly.
            REQUIRE_EQ(t.GetNumFingerprintBits(), full_length - e + 1);
        }
        // Back at the original size, the table holds exactly what it did
        // before, down to the slot.
        REQUIRE_EQ(t.GetNumKeyBits(), original_key_bits);
        REQUIRE_EQ(t.CountFingerprints(), 150);
        REQUIRE(Contents(t) == original);
        REQUIRE_EQ(t.Contract(), FingerprintTable::err_cannot_contract);
    }

    /** Expanding, contracting, inserting, and deleting, all interleaved. */
    static void ResizeMonteCarlo() {
        FingerprintTable t(256, 26, hashmode::Default, 17);
        std::mt19937_64 rng(10);
        std::map<uint64_t, uint64_t> ref;
        std::vector<uint64_t> live;

        for (int32_t step = 0; step < 3000; step++) {
            const uint32_t action = rng() % 100;
            if (action < 5 && t.GetNumFingerprintBits() > 1) {
                REQUIRE_GE(t.Expand(), 0);
                CheckStructure(t);
            }
            else if (action < 8 && t.GetExpansionCount() > 0
                     && t.CountFingerprints() < t.CountSlots() / 2 * FingerprintTable::max_load_factor) {
                REQUIRE_GE(t.Contract(), 0);
                CheckStructure(t);
            }
            else if (action < 70 || live.empty()) {
                if (t.CountFingerprints() + 1 >= t.CountSlots() * FingerprintTable::max_load_factor)
                    continue;
                const uint64_t key = rng() % 10000;
                REQUIRE_EQ(t.Insert(key), 0);
                ref[key]++;
                live.push_back(key);
            }
            else {
                const size_t index = rng() % live.size();
                const uint64_t key = live[index];
                live[index] = live.back();
                live.pop_back();
                // Deleting a key that was inserted always finds a match: the
                // fingerprint of an entry only ever gets shorter, and a
                // shorter fingerprint is a weaker witness, never a wrong one.
                REQUIRE_EQ(t.Delete(key), 0);
                if (--ref[key] == 0)
                    ref.erase(key);
            }

            for (const auto& [key, count] : ref)
                REQUIRE_GE(t.Count(key), count);
        }
        CheckStructure(t);
    }

    static void HashModes() {
        std::mt19937_64 rng(11);
        for (const hashmode mode : {hashmode::Default, hashmode::Invertible, hashmode::None}) {
            FingerprintTable t(256, 18, mode, 1);
            REQUIRE_EQ(t.GetHashMode(), mode);
            std::map<uint64_t, uint64_t> ref;
            for (int32_t i = 0; i < 100; i++) {
                const uint64_t key = rng() & fpt::bitmask(18);
                REQUIRE_EQ(t.Insert(key), 0);
                ref[key]++;
            }
            CheckStructure(t);
            for (const auto& [key, count] : ref) {
                REQUIRE_GE(t.Count(key), count);
                // Neither invertible hashing nor the identity throws bits
                // away, so they cannot produce a false positive here.
                if (mode != hashmode::Default)
                    REQUIRE_EQ(t.Count(key), count);
            }
            for (const auto& [key, count] : ref)
                for (uint64_t i = 0; i < count; i++)
                    REQUIRE_EQ(t.Delete(key), 0);
            REQUIRE_EQ(t.CountFingerprints(), 0);
        }

        // A pre-hashed key skips the hash function.
        FingerprintTable t(256, 18, hashmode::Default, 1);
        const uint64_t key = 12345;
        REQUIRE_EQ(t.Insert(key), 0);
        FingerprintTable u(256, 18, hashmode::None, 1);
        REQUIRE_EQ(u.Insert(MurmurHash64A(&key, sizeof(key), 1)), 0);
        REQUIRE(Contents(t) == Contents(u));
        REQUIRE(t.Contains(MurmurHash64A(&key, sizeof(key), 1), FingerprintTable::flag_key_is_hash));
    }

    static void CopyMoveAndReset() {
        FingerprintTable t(256, 18, hashmode::Default, 1);
        for (int32_t i = 0; i < 50; i++)
            REQUIRE_EQ(t.Insert(i), 0);

        FingerprintTable copy(t);
        REQUIRE(Contents(copy) == Contents(t));
        FingerprintTable moved(std::move(copy));
        REQUIRE(Contents(moved) == Contents(t));

        FingerprintTable assigned(16, 10, hashmode::None, 2);
        assigned = t;
        REQUIRE(Contents(assigned) == Contents(t));
        REQUIRE_EQ(assigned.CountSlots(), t.CountSlots());
        CheckStructure(assigned);

        // The copies are independent of the original.
        t.Reset();
        CheckStructure(t);
        REQUIRE_EQ(t.CountFingerprints(), 0);
        REQUIRE_FALSE(t.Contains(3));
        REQUIRE_EQ(assigned.CountFingerprints(), 50);
        REQUIRE_EQ(moved.CountFingerprints(), 50);
        for (int32_t i = 0; i < 50; i++)
            REQUIRE(moved.Contains(i));

        // The table works again after being reset.
        for (int32_t i = 0; i < 50; i++)
            REQUIRE_EQ(t.Insert(i), 0);
        CheckStructure(t);
        REQUIRE(Contents(t) == Contents(moved));
    }

    /*
     * ------------------------------------------------------------------
     * Stretching (Zeno filter, Section 4.1).
     * ------------------------------------------------------------------
     */

    /** @returns A table with growth coefficient `r`, expanded `expansions` times. */
    static FingerprintTable Stretched(uint64_t nslots, uint64_t key_bits, uint32_t r,
                                      uint32_t expansions, uint32_t seed = 1) {
        FingerprintTable t(nslots, key_bits, hashmode::Default, seed, r);
        for (uint32_t i = 0; i < expansions; i++)
            REQUIRE_GE(t.Expand(), 0);
        return t;
    }

    /**
     * Walks through the toy example of the Zeno filter paper's Figure 5: four
     * slots, a growth coefficient of two, one expansion.
     */
    static void StretchMatchesThePaper() {
        FingerprintTable t(4, 10, hashmode::None, 1, /*growth_coefficient=*/2);
        REQUIRE_EQ(t.GetGrowthCoefficient(), 2);
        REQUIRE_EQ(t.GetEpoch(), 0);
        REQUIRE_EQ(t.CountSlots(), 4);
        REQUIRE_EQ(t.GetBaseSlotCount(), 4);
        // At epoch 0 nothing is stretched.
        for (uint64_t b = 0; b < 4; b++)
            REQUIRE_EQ(t.stretch(b), b);

        REQUIRE_EQ(t.Expand(), 0);
        REQUIRE_EQ(t.GetEpoch(), 1);
        REQUIRE_EQ(t.GetPeriodCount(), 0);
        REQUIRE_EQ(t.GetExpansionCount(), 1);
        // The base address space is untouched; only the slots it is spread
        // over grow, by a factor of 2^(1/2): floor(4 * 1.414...) = 5.
        REQUIRE_EQ(t.GetBaseSlotCount(), 4);
        REQUIRE_EQ(t.CountSlots(), 5);
        // Figure 5: buckets 00, 01, 10, and 11 land on 00, 01, 010, and 100.
        REQUIRE_EQ(t.stretch(0b00), 0b000);
        REQUIRE_EQ(t.stretch(0b01), 0b001);
        REQUIRE_EQ(t.stretch(0b10), 0b010);
        REQUIRE_EQ(t.stretch(0b11), 0b100);
        for (uint64_t b = 0; b < 4; b++)
            REQUIRE_EQ(t.unstretch(t.stretch(b)), b);

        // The second expansion ends the period, so the table returns to a
        // power of two and every fingerprint gives up a bit.
        const uint64_t full_length = t.GetNumFingerprintBits();
        REQUIRE_EQ(t.Expand(), 0);
        REQUIRE_EQ(t.GetNumFingerprintBits(), full_length - 1);
        REQUIRE_EQ(t.GetEpoch(), 0);
        REQUIRE_EQ(t.GetPeriodCount(), 1);
        REQUIRE_EQ(t.GetExpansionCount(), 2);
        REQUIRE_EQ(t.CountSlots(), 8);
        REQUIRE_EQ(t.GetBaseSlotCount(), 8);
    }

    /**
     * The stretch map is what lets a run be found in the enlarged table, so it
     * has to be strictly increasing (runs never cross, and distinct buckets
     * stay distinct), stay inside the table, and be exactly invertible.
     */
    static void StretchIsAStrictlyIncreasingBijection() {
        for (const uint32_t r : {1u, 2u, 3u, 5u, 8u}) {
            for (uint32_t e = 0; e < r; e++) {
                FingerprintTable t = Stretched(1024, 30, r, e);
                REQUIRE_EQ(t.GetEpoch(), e);
                REQUIRE_EQ(t.GetPeriodCount(), 0);
                REQUIRE_EQ(t.GetBaseSlotCount(), 1024);

                uint64_t previous = 0;
                for (uint64_t b = 0; b < t.GetBaseSlotCount(); b++) {
                    const uint64_t slot = t.stretch(b);
                    if (b > 0)
                        REQUIRE_GT(slot, previous);
                    // Every bucket of the base address space has a slot of its
                    // own inside the table.
                    REQUIRE_GE(slot, b);
                    REQUIRE_LT(slot, t.CountSlots());
                    REQUIRE_EQ(t.unstretch(slot), b);
                    previous = slot;
                }

                // The stretched size is the same map applied to the base size,
                // i.e. the table grows by 2^(e/r).
                const long double expected = floorl(1024.0L * exp2l(static_cast<long double>(e) / r));
                REQUIRE_LE(std::abs(static_cast<long double>(t.CountSlots()) - expected), 1.0L);
            }
        }
    }

    /**
     * With a growth coefficient of `r` the table takes `r` expansions to
     * double, growing by 2^(1/r) at a time, and sheds a fingerprint bit only
     * on the last of them.
     */
    /**
     * A caller driving expansion from a size function has to know what the
     * next expansion would cost before making it, so the projected slot count
     * has to agree with what `Expand` and `Contract` actually produce -- at
     * every epoch, floors and all.
     */
    static void PredictsTheSlotCountAfterAResize() {
        for (const uint32_t r : {1u, 2u, 3u, 5u}) {
            FingerprintTable t(1000, 34, hashmode::Default, 90 + r, r);
            std::mt19937_64 rng(90 + r);
            for (int32_t i = 0; i < 200; i++)
                REQUIRE_EQ(t.Insert(rng()), 0);

            // Nothing to give a bit back to at the original size.
            REQUIRE_EQ(t.CountSlotsAfterContraction(), t.CountSlots());

            std::vector<uint64_t> sizes{t.CountSlots()};
            for (uint32_t i = 0; i < 2 * r + 1; i++) {
                const uint64_t predicted = t.CountSlotsAfterExpansion();
                REQUIRE_GT(predicted, t.CountSlots());
                REQUIRE_GE(t.Expand(), 0);
                REQUIRE_EQ(t.CountSlots(), predicted);
                // The way back down is predicted just as exactly.
                REQUIRE_EQ(t.CountSlotsAfterContraction(), sizes.back());
                sizes.push_back(t.CountSlots());
            }
            while (t.GetExpansionCount() > 0) {
                sizes.pop_back();
                const uint64_t predicted = t.CountSlotsAfterContraction();
                REQUIRE_GE(t.Contract(), 0);
                REQUIRE_EQ(t.CountSlots(), predicted);
                REQUIRE_EQ(t.CountSlots(), sizes.back());
            }
        }
    }

    static void StretchingGrowsGradually() {
        for (const uint32_t r : {1u, 2u, 3u, 4u, 7u}) {
            FingerprintTable t(4096, 34, hashmode::Default, r, r);
            const uint64_t full_length = t.GetNumFingerprintBits();
            const uint64_t original_slots = t.CountSlots();
            const uint64_t original_key_bits = t.GetNumKeyBits();
            const long double step = exp2l(1.0L / r);

            std::mt19937_64 rng(20 + r);
            std::map<uint64_t, uint64_t> ref;
            for (int32_t i = 0; i < 500; i++) {
                const uint64_t key = rng();
                REQUIRE_EQ(t.Insert(key), 0);
                ref[key]++;
            }

            for (uint32_t i = 1; i <= 3 * r; i++) {
                const uint64_t slots_before = t.CountSlots();
                REQUIRE_EQ(t.Expand(), 500);
                CheckStructure(t);

                REQUIRE_EQ(t.GetExpansionCount(), i);
                REQUIRE_EQ(t.GetEpoch(), i % r);
                REQUIRE_EQ(t.GetPeriodCount(), i / r);
                // Every expansion grows the table by the same factor, whether
                // or not it is the one that ends the period.
                const long double ratio = static_cast<long double>(t.CountSlots()) / slots_before;
                REQUIRE_GT(ratio, step - 0.01L);
                REQUIRE_LT(ratio, step + 0.01L);
                // A period returns the table to a power-of-two multiple of the
                // size it started at.
                if (i % r == 0)
                    REQUIRE_EQ(t.CountSlots(), original_slots << (i / r));

                // A fingerprint bit is spent per period, not per expansion,
                // and it comes out of the fingerprint rather than out of a
                // wider hash: `key_bits` never moves.
                REQUIRE_EQ(t.GetNumKeyBits(), original_key_bits);
                REQUIRE_EQ(t.GetNumFingerprintBits(), full_length - i / r);
                REQUIRE_EQ(t.GetBitsPerSlot(), full_length - i / r);
                for (const auto& [key, count] : ref) {
                    REQUIRE_GE(t.Count(key), count);
                    REQUIRE(t.Contains(key));
                }
                for (auto it = t.begin(); it != t.end(); ++it)
                    REQUIRE_LT(it.bucket(), t.CountSlots());
            }
        }
    }

    /** Contraction undoes a stretch just as exactly as it undoes a doubling. */
    static void StretchedContractionInvertsExpansion() {
        for (const uint32_t r : {2u, 3u, 5u}) {
            FingerprintTable t(512, 28, hashmode::Default, r, r);
            std::mt19937_64 rng(30 + r);
            std::map<uint64_t, uint64_t> ref;
            for (int32_t i = 0; i < 300; i++) {
                const uint64_t key = rng();
                REQUIRE_EQ(t.Insert(key), 0);
                ref[key]++;
            }
            const auto original = Contents(t);
            const uint64_t original_slots = t.CountSlots();
            const uint64_t original_key_bits = t.GetNumKeyBits();
            REQUIRE_EQ(t.Contract(), FingerprintTable::err_cannot_contract);

            const uint32_t steps = 2 * r + 1;
            std::vector<uint64_t> sizes;
            for (uint32_t i = 0; i < steps; i++) {
                REQUIRE_EQ(t.Expand(), 300);
                sizes.push_back(t.CountSlots());
            }
            // Contracting retraces the sizes the table grew through, one for
            // one, all the way back.
            for (uint32_t i = steps; i > 0; i--) {
                REQUIRE_EQ(t.CountSlots(), sizes[i - 1]);
                REQUIRE_EQ(t.GetExpansionCount(), i);
                REQUIRE_EQ(t.Contract(), 300);
                CheckStructure(t);
                for (const auto& [key, count] : ref)
                    REQUIRE_GE(t.Count(key), count);
            }
            REQUIRE_EQ(t.CountSlots(), original_slots);
            REQUIRE_EQ(t.GetNumKeyBits(), original_key_bits);
            REQUIRE_EQ(t.GetEpoch(), 0);
            REQUIRE_EQ(t.GetPeriodCount(), 0);
            REQUIRE(Contents(t) == original);
            REQUIRE_EQ(t.Contract(), FingerprintTable::err_cannot_contract);
        }
    }

    /**
     * Stretching is what the growth coefficient is for: expanding by 2^(1/r)
     * instead of 2 leaves the table far less empty right after it grows.
     */
    static void StretchingCurbsSpaceAmplification() {
        double previous_worst = 0.0;
        for (const uint32_t r : {1u, 2u, 4u, 8u}) {
            FingerprintTable t(1024, 40, hashmode::Default, 1, r);
            t.SetAutoExpand(true);
            std::mt19937_64 rng(40 + r);
            std::vector<uint64_t> keys;

            // The persistent space amplification is the reciprocal of the
            // *worst* load factor the table ever sits at, which is the one
            // right after it grows. Sample it across enough insertions to
            // drive several expansions.
            double worst_load = 1.0;
            for (int32_t i = 0; i < 20000; i++) {
                const uint64_t key = rng();
                REQUIRE_EQ(t.Insert(key), 0);
                keys.push_back(key);
                if (t.GetExpansionCount() > 0)
                    worst_load = std::min(worst_load, t.LoadFactor());
            }
            CheckStructure(t);
            REQUIRE_EQ(t.CountFingerprints(), keys.size());
            for (const uint64_t key : keys)
                REQUIRE(t.Contains(key));
            REQUIRE_LE(t.LoadFactor(), FingerprintTable::max_load_factor);

            // Expanding by 2^(1/r) at a load factor of alpha bounds the space
            // amplification by 2^(1/r)/alpha, i.e. the load factor never falls
            // below alpha/2^(1/r).
            REQUIRE_GT(worst_load, FingerprintTable::max_load_factor / exp2(1.0 / r) - 0.02);
            REQUIRE_LE(worst_load, FingerprintTable::max_load_factor);
            // A larger growth coefficient wastes strictly less space.
            REQUIRE_GT(worst_load, previous_worst);
            previous_worst = worst_load;
        }
    }

    /**
     * A fingerprint bit is spent when a period ends, and only then: a
     * Stretching step hands out no bucket-index bit to pay for one.
     */
    static void StretchingSpendsABitOncePerPeriod() {
        const uint32_t r = 3;
        FingerprintTable t(64, 14, hashmode::Default, 3, r);
        const uint64_t full_length = t.GetNumFingerprintBits();
        REQUIRE_EQ(full_length, 8);

        std::mt19937_64 rng(31);
        std::vector<uint64_t> keys;
        for (int32_t i = 0; i < 30; i++) {
            const uint64_t key = rng();
            REQUIRE_EQ(t.Insert(key), 0);
            keys.push_back(key);
        }

        for (uint32_t i = 1; i < full_length * r; i++) {
            REQUIRE_EQ(t.Expand(), keys.size());
            CheckStructure(t);
            REQUIRE_EQ(t.GetPeriodCount(), i / r);
            // The length follows the periods, not the expansions.
            REQUIRE_EQ(t.GetNumFingerprintBits(), full_length - i / r);
            REQUIRE_EQ(t.CountFingerprints(), keys.size());
            for (const uint64_t key : keys)
                REQUIRE(t.Contains(key));
        }
    }

    /** Expanding, contracting, inserting, and deleting a stretched table. */
    static void StretchedResizeMonteCarlo() {
        for (const uint32_t r : {2u, 3u, 6u}) {
            FingerprintTable t(256, 30, hashmode::Default, r, r);
            std::mt19937_64 rng(60 + r);
            std::map<uint64_t, uint64_t> ref;
            std::vector<uint64_t> live;

            for (int32_t step = 0; step < 4000; step++) {
                const uint32_t action = rng() % 100;
                if (action < 5 && t.GetNumFingerprintBits() > 1) {
                    REQUIRE_GE(t.Expand(), 0);
                    CheckStructure(t);
                }
                else if (action < 9 && t.GetExpansionCount() > 0
                         && t.CountFingerprints() < t.CountSlots() / 2
                                                        * FingerprintTable::max_load_factor) {
                    REQUIRE_GE(t.Contract(), 0);
                    CheckStructure(t);
                }
                else if (action < 70 || live.empty()) {
                    if (t.CountFingerprints() + 1 >= t.CountSlots() * FingerprintTable::max_load_factor)
                        continue;
                    const uint64_t key = rng() % 10000;
                    REQUIRE_EQ(t.Insert(key), 0);
                    ref[key]++;
                    live.push_back(key);
                }
                else {
                    const size_t index = rng() % live.size();
                    const uint64_t key = live[index];
                    live[index] = live.back();
                    live.pop_back();
                    REQUIRE_EQ(t.Delete(key), 0);
                    if (--ref[key] == 0)
                        ref.erase(key);
                }

                for (const auto& [key, count] : ref)
                    REQUIRE_GE(t.Count(key), count);
            }
            CheckStructure(t);

            // Whatever state it ended in, the table still drains completely.
            for (const uint64_t key : live)
                REQUIRE_EQ(t.Delete(key), 0);
            CheckStructure(t);
            REQUIRE_EQ(t.CountFingerprints(), 0);
        }
    }

    /** The growth coefficient and epoch survive a copy, a move, and a reset. */
    static void StretchedCopyMoveAndReset() {
        FingerprintTable t(256, 24, hashmode::Default, 1, /*growth_coefficient=*/4);
        for (int32_t i = 0; i < 50; i++)
            REQUIRE_EQ(t.Insert(i), 0);
        REQUIRE_EQ(t.Expand(), 50);
        REQUIRE_EQ(t.Expand(), 50);
        REQUIRE_EQ(t.GetEpoch(), 2);

        FingerprintTable copy(t);
        REQUIRE_EQ(copy.GetGrowthCoefficient(), 4);
        REQUIRE_EQ(copy.GetEpoch(), 2);
        REQUIRE_EQ(copy.CountSlots(), t.CountSlots());
        REQUIRE(Contents(copy) == Contents(t));

        FingerprintTable assigned(16, 10, hashmode::None, 2);
        assigned = t;
        REQUIRE_EQ(assigned.GetGrowthCoefficient(), 4);
        REQUIRE_EQ(assigned.GetEpoch(), 2);
        REQUIRE(Contents(assigned) == Contents(t));
        CheckStructure(assigned);

        // A reset empties the table but leaves its shape alone, so the same
        // keys land in the same slots again.
        const auto original = Contents(t);
        t.Reset();
        REQUIRE_EQ(t.GetGrowthCoefficient(), 4);
        REQUIRE_EQ(t.GetEpoch(), 2);
        REQUIRE_EQ(t.CountFingerprints(), 0);
        // No period has ended, so the fingerprints were never shortened and a
        // plain re-insertion reproduces them exactly.
        REQUIRE_EQ(t.GetPeriodCount(), 0);
        for (int32_t i = 0; i < 50; i++)
            REQUIRE_EQ(t.Insert(i), 0);
        REQUIRE(Contents(t) == original);
    }

    /** Stretching an address space that did not start at a power of two. */
    static void StretchedNonPowerOfTwoSize() {
        std::mt19937_64 rng(70);
        for (const uint64_t nslots : {100ULL, 300ULL, 1000ULL}) {
            const uint32_t r = 3;
            FingerprintTable t(nslots, 26, hashmode::Default, 5, r);
            REQUIRE_EQ(t.CountSlots(), nslots);
            std::map<uint64_t, uint64_t> ref;
            for (uint64_t i = 0; i < nslots / 2; i++) {
                const uint64_t key = rng();
                REQUIRE_EQ(t.Insert(key), 0);
                ref[key]++;
            }

            uint64_t previous_slots = nslots;
            for (uint32_t i = 1; i <= 2 * r; i++) {
                REQUIRE_GT(t.Expand(), 0);
                CheckStructure(t);
                REQUIRE_GT(t.CountSlots(), previous_slots);
                previous_slots = t.CountSlots();
                REQUIRE_EQ(t.GetBaseSlotCount(), nslots << (i / r));
                if (i % r == 0)
                    REQUIRE_EQ(t.CountSlots(), nslots << (i / r));
                for (auto it = t.begin(); it != t.end(); ++it)
                    REQUIRE_LT(it.bucket(), t.CountSlots());
                for (const auto& [key, count] : ref)
                    REQUIRE_GE(t.Count(key), count);
            }

            for (uint32_t i = 0; i < 2 * r; i++)
                REQUIRE_GT(t.Contract(), 0);
            CheckStructure(t);
            REQUIRE_EQ(t.CountSlots(), nslots);
            for (const auto& [key, count] : ref)
                for (uint64_t i = 0; i < count; i++)
                    REQUIRE_EQ(t.Delete(key), 0);
            REQUIRE_EQ(t.CountFingerprints(), 0);
        }
    }

    /** The table starts at a size that need not be a power of two. */
    static void NonPowerOfTwoSize() {
        std::mt19937_64 rng(12);
        for (const uint64_t nslots : {100ULL, 300ULL, 1000ULL}) {
            FingerprintTable t(nslots, 22, hashmode::Default, 5);
            REQUIRE_EQ(t.CountSlots(), nslots);
            std::map<uint64_t, uint64_t> ref;
            for (uint64_t i = 0; i < nslots / 2; i++) {
                const uint64_t key = rng();
                REQUIRE_EQ(t.Insert(key), 0);
                ref[key]++;
            }
            CheckStructure(t);
            for (auto it = t.begin(); it != t.end(); ++it)
                REQUIRE_LT(it.bucket(), nslots);

            REQUIRE_GT(t.Expand(), 0);
            CheckStructure(t);
            REQUIRE_EQ(t.CountSlots(), 2 * nslots);
            for (auto it = t.begin(); it != t.end(); ++it)
                REQUIRE_LT(it.bucket(), 2 * nslots);
            for (const auto& [key, count] : ref)
                REQUIRE_GE(t.Count(key), count);

            REQUIRE_GT(t.Contract(), 0);
            CheckStructure(t);
            REQUIRE_EQ(t.CountSlots(), nslots);
            for (const auto& [key, count] : ref) {
                REQUIRE_GE(t.Count(key), count);
                for (uint64_t i = 0; i < count; i++)
                    REQUIRE_EQ(t.Delete(key), 0);
            }
            REQUIRE_EQ(t.CountFingerprints(), 0);
        }
    }
};

}   // namespace sublime

using sublime::FingerprintTableTest;

TEST_SUITE("FingerprintTable") {
    TEST_CASE("slot read/write") {
        FingerprintTableTest::SlotReadWrite();
    }

    TEST_CASE("hashing") {
        FingerprintTableTest::Hashing();
    }

    TEST_CASE("insert and count") {
        FingerprintTableTest::InsertAndCount();
    }

    TEST_CASE("delete removes the match") {
        FingerprintTableTest::DeleteRemovesMatch();
    }

    TEST_CASE("insert and delete monte carlo") {
        FingerprintTableTest::InsertDeleteMonteCarlo();
    }

    TEST_CASE("high load factor") {
        FingerprintTableTest::HighLoadFactor();
    }

    TEST_CASE("auto expand") {
        FingerprintTableTest::AutoExpand();
    }

    TEST_CASE("expansion") {
        FingerprintTableTest::Expansion();
    }

    TEST_CASE("growth stops when fingerprints run out") {
        FingerprintTableTest::GrowthStopsWhenFingerprintsRunOut();
    }

    TEST_CASE("contraction inverts expansion") {
        FingerprintTableTest::ContractionInvertsExpansion();
    }

    TEST_CASE("resize monte carlo") {
        FingerprintTableTest::ResizeMonteCarlo();
    }

    TEST_CASE("hash modes") {
        FingerprintTableTest::HashModes();
    }

    TEST_CASE("copy, move, and reset") {
        FingerprintTableTest::CopyMoveAndReset();
    }

    TEST_CASE("non power of two size") {
        FingerprintTableTest::NonPowerOfTwoSize();
    }

    TEST_CASE("stretch matches the paper") {
        FingerprintTableTest::StretchMatchesThePaper();
    }

    TEST_CASE("stretch is a strictly increasing bijection") {
        FingerprintTableTest::StretchIsAStrictlyIncreasingBijection();
    }

    TEST_CASE("predicts the slot count after a resize") {
        FingerprintTableTest::PredictsTheSlotCountAfterAResize();
    }

    TEST_CASE("stretching grows gradually") {
        FingerprintTableTest::StretchingGrowsGradually();
    }

    TEST_CASE("stretched contraction inverts expansion") {
        FingerprintTableTest::StretchedContractionInvertsExpansion();
    }

    TEST_CASE("stretching curbs space amplification") {
        FingerprintTableTest::StretchingCurbsSpaceAmplification();
    }

    TEST_CASE("stretching spends a fingerprint bit once per period") {
        FingerprintTableTest::StretchingSpendsABitOncePerPeriod();
    }

    TEST_CASE("stretched resize monte carlo") {
        FingerprintTableTest::StretchedResizeMonteCarlo();
    }

    TEST_CASE("stretched copy, move, and reset") {
        FingerprintTableTest::StretchedCopyMoveAndReset();
    }

    TEST_CASE("stretched non power of two size") {
        FingerprintTableTest::StretchedNonPowerOfTwoSize();
    }
}
