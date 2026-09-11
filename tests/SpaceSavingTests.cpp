#include <algorithm>
#include <random>
#include <unordered_map>
#include <vector>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>

#include "SpaceSaving.hpp"

namespace sublime {

/** White-box tests for `SpaceSaving`, a `friend` so they can walk its lists. */
class SpaceSavingTest {
public:
    /**
     * The structural invariants of the Stream-Summary:
     *  - buckets are in strictly ascending value order, none empty;
     *  - every monitor's `parent`/`prev` links are consistent;
     *  - the index and the monitors are in bijection;
     *  - the counts sum to the stream length (every insertion adds exactly one);
     *  - the head bucket is the minimum.
     */
    static void CheckStructure(const SpaceSaving& ss, uint64_t stream_length) {
        uint64_t monitors = 0, total_value = 0, buckets = 0;
        bool first = true;
        uint64_t prev_value = 0;
        for (SpaceSaving::Bucket *b = ss.head_; b != nullptr; b = b->next) {
            REQUIRE(b->child != nullptr);                       // No empty bucket.
            if (!first)
                REQUIRE_LT(prev_value, b->value);               // Strictly ascending.
            first = false;
            prev_value = b->value;

            SpaceSaving::Counter *prev = nullptr;
            for (SpaceSaving::Counter *c = b->child; c != nullptr; c = c->next) {
                REQUIRE_EQ(c->parent, b);
                REQUIRE_EQ(c->prev, prev);
                REQUIRE_GE(b->value, c->error);                 // value >= error.
                prev = c;
                monitors++;
                total_value += b->value;
            }
            buckets++;
        }
        REQUIRE_EQ(buckets, ss.bucket_count_);
        REQUIRE_EQ(monitors, ss.index_.size());
        for (const auto& [key, counter] : ss.index_)
            REQUIRE_EQ(counter->elem, key);
        REQUIRE_EQ(total_value, stream_length);                 // Counts sum to N.
        if (ss.head_ != nullptr)
            for (SpaceSaving::Bucket *b = ss.head_; b != nullptr; b = b->next)
                REQUIRE_GE(b->value, ss.head_->value);
    }

    static uint64_t MinValue(const SpaceSaving& ss) {
        return ss.head_ == nullptr ? 0 : ss.head_->value;
    }
};


static void RunGuarantees(uint64_t capacity, uint32_t seed, uint64_t universe_size,
                          uint64_t stream_length) {
    SpaceSaving ss(capacity);
    std::unordered_map<uint64_t, uint64_t> exact;

    std::mt19937_64 rng(seed);
    const auto draw = [&]() {
        uint64_t m = rng() % universe_size;
        for (int i = 0; i < 3; i++)
            m = std::min(m, rng() % universe_size);
        return m;
    };

    for (uint64_t t = 0; t < stream_length; t++) {
        const uint64_t key = draw();
        ss.Insert(key);
        exact[key]++;
        if (t % (stream_length / 6 + 1) == 0)
            SpaceSavingTest::CheckStructure(ss, t + 1);
    }
    SpaceSavingTest::CheckStructure(ss, stream_length);

    const uint64_t distinct = exact.size();
    REQUIRE_EQ(ss.CountMonitored(), std::min(distinct, capacity));

    const uint64_t min_value = SpaceSavingTest::MinValue(ss);
    if (distinct >= capacity)
        REQUIRE_LE(min_value, stream_length / capacity);        // Space-Saving bound.

    for (const auto& [key, count] : exact) {
        if (ss.IsMonitored(key)) {
            const uint64_t est = ss.Query(key);
            const uint64_t err = ss.QueryError(key);
            REQUIRE_GE(est, count);                             // Over-estimate.
            REQUIRE_LE(est - err, count);                       // est - err <= true <= est.
        }
        else {
            REQUIRE_LE(count, min_value);                       // Unmonitored => small.
        }
    }
}


TEST_CASE("space-saving guarantees hold across configurations") {
    RunGuarantees(/*capacity=*/64, /*seed=*/1, /*universe=*/5000, /*stream=*/300000);
    RunGuarantees(256, 2, 20000, 600000);
    RunGuarantees(1000, 3, 100000, 800000);
    RunGuarantees(7, 4, 200, 40000);                            // Tiny.
}

TEST_CASE("space-saving admits exactly, then over-estimates") {
    SpaceSaving ss(4);
    // Four distinct keys fill the summary exactly (error 0 each).
    for (uint64_t k = 0; k < 4; k++)
        for (int r = 0; r < static_cast<int>(k) + 1; r++)
            ss.Insert(k);
    REQUIRE_EQ(ss.CountMonitored(), 4);
    for (uint64_t k = 0; k < 4; k++) {
        REQUIRE_EQ(ss.Query(k), k + 1);
        REQUIRE_EQ(ss.QueryError(k), 0);
    }
    // A fifth key steals the minimum (key 0, count 1): it lands at count 2, error 1.
    ss.Insert(100);
    REQUIRE_FALSE(ss.IsMonitored(0));
    REQUIRE_EQ(ss.Query(100), 2);
    REQUIRE_EQ(ss.QueryError(100), 1);
    SpaceSavingTest::CheckStructure(ss, 1 + 2 + 3 + 4 + 1);
}

TEST_CASE("space-saving reset empties and reuses") {
    SpaceSaving ss(128);
    std::mt19937_64 rng(9);
    for (uint64_t t = 0; t < 50000; t++)
        ss.Insert(std::min(rng() % 3000, rng() % 3000));
    REQUIRE_GT(ss.CountMonitored(), 0);
    ss.Reset();
    REQUIRE_EQ(ss.CountMonitored(), 0);
    REQUIRE_EQ(ss.GetStreamLength(), 0);
    REQUIRE_EQ(ss.Query(5), 0);
    for (uint64_t t = 0; t < 50000; t++)
        ss.Insert(std::min(rng() % 2000, rng() % 2000));
    SpaceSavingTest::CheckStructure(ss, 50000);
}

}   // namespace sublime
