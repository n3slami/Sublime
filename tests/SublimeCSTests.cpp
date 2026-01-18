#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>
#include <assert.h>

#include "SublimeCS.hpp"
#include "CS.hpp"

class SublimeCSTest {
public:
    static void CounterRW1() {
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        SublimeCS<> sketch(10, 10, f, 1);

        sketch.set_counter(sketch.sketches.back(), 2, 5);
        sketch.set_counter(sketch.sketches.back(), 2, 129);
        sketch.set_counter(sketch.sketches.back(), 0, 130);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 130);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 0);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), 129);

        sketch.set_counter(sketch.sketches.back(), 0, 3);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 3);

        sketch.set_counter(sketch.sketches.back(), 1, 1500);
        sketch.set_counter(sketch.sketches.back(), 0, 900);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 900);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 1500);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), 129);

        for (int i = 3; i < 20; i++)
            sketch.set_counter(sketch.sketches.back(), i, 65);
        sketch.set_counter(sketch.sketches.back(), 0, 10);
        assert(sketch.get_counter(sketch.sketches.back(), 0) == 10);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 10);

        sketch.set_counter(sketch.sketches.back(), 0, 900);
        sketch.set_counter(sketch.sketches.back(), 1, 100);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 900);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 100);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), 129);

        sketch.set_counter(sketch.sketches.back(), 1, 1500);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 900);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 1500);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), 129);

        for (int i = 20; i < 30; i++)
            sketch.set_counter(sketch.sketches.back(), i, 65);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 900);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 1500);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), 129);
        for (int i = 3; i < 20; i++)
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 65);
        for (int i = 30; i < 60; i++)
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 0);
    }

    static void CounterRW2() {
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        SublimeCS<> sketch(10, 10, f, 1);

        for (int i = 0; i < 25; i++)
            sketch.set_counter(sketch.sketches.back(), i, 65);
        sketch.set_counter(sketch.sketches.back(), 0, 1000000000);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 1000000000);
        for (int i = 1; i < 25; i++)
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 65);
        for (int i = 25; i < 100; i++)
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 0);
    }

    static void CounterIncrement1() {
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        SublimeCS<> sketch(10, 10, f, 1);

        for (int i = 0; i < 63; i++) {
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), i);
            sketch.increment_counter(sketch.sketches.back(), 1);
        }
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 63);
        sketch.increment_counter(sketch.sketches.back(), 1);
        for (int i = 0; i < 63; i++) {
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 + i);
            sketch.increment_counter(sketch.sketches.back(), 1);
        }
        sketch.increment_counter(sketch.sketches.back(), 1);

        for (int i = 0; i < 64; i++)
            sketch.increment_counter(sketch.sketches.back(), 0);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 0), 64);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 128);

        for (int i = 2; i < 28; i++)
            sketch.set_counter(sketch.sketches.back(), i, 64 * 3 - 1);
        sketch.increment_counter(sketch.sketches.back(), 2);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), 64 * 3);
        for (int i = 64 * 3; i < 10000; i++) {
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), i);
            sketch.increment_counter(sketch.sketches.back(), 2);
        }
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 2), 10000);
    }

    static void CounterIncrement2() {
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        SublimeCS<> sketch(10, 10, f, 1);

        for (int i = 0; i < 27; i++)
            sketch.set_counter(sketch.sketches.back(), i, 64 * 3 - 1);
        sketch.increment_counter(sketch.sketches.back(), 25);
        for (int i = 0; i < 27; i++)
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 64 * 3 - (i != 25));
        for (int i = 64 * 3; i < 64 * 9; i++) {
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 25), i);
            sketch.increment_counter(sketch.sketches.back(), 25);
        }
        //PrintSketch(sketch);
        for (int i = 64 * 9; i < 64 * 27; i++) {
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 25), i);
            sketch.increment_counter(sketch.sketches.back(), 25);
        }
    }

    static void CounterDecrement() {
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        SublimeCS<> sketch(10, 10, f, 1);

        for (int i = 0; i < 25; i++)
            sketch.set_counter(sketch.sketches.back(), i, 65);
        sketch.set_counter(sketch.sketches.back(), 1, 64 * 9);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 9);
        sketch.decrement_counter(sketch.sketches.back(), 1);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 9 - 1);
        for (int i = 64 * 9 - 1; i > 64 * 3; i--) {
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), i);
            sketch.decrement_counter(sketch.sketches.back(), 1);
        }
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 3);
        sketch.decrement_counter(sketch.sketches.back(), 1);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 3 - 1);

        sketch.decrement_counter(sketch.sketches.back(), 23);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 23), 64);
        sketch.decrement_counter(sketch.sketches.back(), 23);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 23), 63);

        for (int i = 25; i < 99; i++)
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 0);
        sketch.set_counter(sketch.sketches.back(), 40, 10000000);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 40), 10000000);

        sketch.decrement_counter(sketch.sketches.back(), 40);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 40), 9999999);

        sketch.decrement_counter(sketch.sketches.back(), 22);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 22), 64);

        sketch.decrement_counter(sketch.sketches.back(), 22);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 22), 63);
    }

    static void CounterNegative() {
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        SublimeCS<> sketch(10, 10, f, 1);

        for (int i = 0; i < 25; i++)
            sketch.set_counter(sketch.sketches.back(), i, (i & 1) ? 65 : -65);
        sketch.set_counter(sketch.sketches.back(), 1, 64 * 9);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 9);
        sketch.decrement_counter(sketch.sketches.back(), 1);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 9 - 1);
        for (int i = 64 * 9 - 1; i > 64 * 3; i--) {
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), i);
            sketch.decrement_counter(sketch.sketches.back(), 1);
        }
        assert(sketch.get_counter(sketch.sketches.back(), 1) == 64 * 3);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 3);
        sketch.decrement_counter(sketch.sketches.back(), 1);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 1), 64 * 3 - 1);

        sketch.decrement_counter(sketch.sketches.back(), 26);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 26), -1);

        sketch.decrement_counter(sketch.sketches.back(), 26);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 26), -2);

        sketch.increment_counter(sketch.sketches.back(), 26);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 26), -1);

        sketch.increment_counter(sketch.sketches.back(), 26);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 26), 0);

        sketch.increment_counter(sketch.sketches.back(), 26);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 26), 1);

        sketch.decrement_counter(sketch.sketches.back(), 26);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 26), 0);

        sketch.decrement_counter(sketch.sketches.back(), 23);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 23), 64);

        sketch.decrement_counter(sketch.sketches.back(), 23);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 23), 63);

        for (int i = 25; i < 99; i++)
            REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 0);
        sketch.set_counter(sketch.sketches.back(), 40, -10000000);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 40), -10000000);

        sketch.increment_counter(sketch.sketches.back(), 40);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 40), -9999999);

        sketch.increment_counter(sketch.sketches.back(), 22);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 22), -64);

        sketch.increment_counter(sketch.sketches.back(), 22);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 22), -63);
    }

    static void ExpandAndContract() {
        const uint32_t init_col_count = 10000;
        const uint32_t init_row_count = 10;
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        SublimeCS<> sketch(init_col_count, init_row_count, f, 1);

        const uint32_t key = 1;
        const uint32_t one_count = f(init_col_count);
        for (int i = 0; i < one_count; i++)
            sketch.Insert(key);
        sketch.FlushPrefetchQueue();

        assert(sketch.Query(key) == one_count);
        assert(sketch.Query(key + 1) == 0);

        int32_t pos = -1;
        for (int i = 0; i < sketch.col_count; i++) {
            if (sketch.get_counter(sketch.sketches.back(), i) > 0) {
                pos = i;
                REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), one_count);
            }
        }
        REQUIRE_NE(pos, -1);
        const int32_t pos_alt = init_row_count * init_col_count + pos;

        sketch.Insert(key);
        sketch.FlushPrefetchQueue();

        REQUIRE_EQ(sketch.col_count, 2 * init_col_count);
        REQUIRE_EQ(sketch.row_count, init_row_count);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), pos), one_count + 1);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), pos_alt), one_count);

        sketch.Delete(key);
        sketch.Delete(key);
        REQUIRE_EQ(sketch.col_count, init_col_count);
        REQUIRE_EQ(sketch.row_count, init_row_count);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), pos), one_count - 1);
    }

    static void Reallocate() {
        const uint32_t N = 2e7;
        const uint32_t rng_seed = 1380;
        const uint32_t init_col_count = 1000;
        const uint32_t init_row_count = 3;
        const uint32_t seed_gen_seed = 1;
        auto f = [](double x) { return std::numeric_limits<uint64_t>::max(); };
        SublimeCS<> sketch(init_col_count, init_row_count, f, seed_gen_seed);

        std::mt19937_64 rng(rng_seed);

        std::vector<uint64_t> keys;
        for (uint32_t i = 0; i < N; i++) {
            const uint64_t key = rng();
            sketch.Insert(key);
        }
        REQUIRE_EQ(sketch.sketches.back()->counter_per_cache_line, 45);
        REQUIRE_EQ(sketch.sketches.back()->stub_size, 8);
    }

    static void MonteCarlo() {
        const uint32_t N = 2e7;
        const uint32_t check_period = 2e6;
        const uint32_t rng_seed = 1380;
        const uint32_t init_col_count = 10000;
        const uint32_t init_row_count = 3;
        const uint32_t seed_gen_seed = 1;
        auto f = [](double x) { return static_cast<uint64_t>(x); };
        SublimeCS<> sketch(init_col_count, init_row_count, f, seed_gen_seed);
        CS<int32_t> expected_sketch(init_col_count, init_row_count, f, seed_gen_seed, true);

        std::mt19937_64 rng(rng_seed);

        std::vector<uint64_t> keys;
        for (uint32_t i = 0; i < N; i++) {
            const uint64_t key = rng();
            sketch.Insert(key);
            expected_sketch.Insert(key);
            if (i % check_period == 0) {
                sketch.FlushPrefetchQueue();
                REQUIRE_EQ(sketch.counter_count, expected_sketch.counter_count);
                REQUIRE_EQ(sketch.col_count, expected_sketch.col_count);
                REQUIRE_EQ(sketch.row_count, expected_sketch.row_count);
                REQUIRE_EQ(sketch.n, expected_sketch.n);
                REQUIRE_EQ(sketch.expansion_lim, expected_sketch.expansion_lim);
                REQUIRE_EQ(sketch.contraction_lim, expected_sketch.contraction_lim);
                for (uint32_t j = 0; j < sketch.sketches.size(); j++) {
                    for (uint32_t k = 0; k < sketch.sketches[j]->counter_count; k++)
                        REQUIRE_EQ(sketch.get_counter(sketch.sketches[j], k),
                                reinterpret_cast<const int32_t *>(expected_sketch.sketches[j])[k]);
                }
            }
            keys.push_back(key);
        }

        std::shuffle(keys.begin(), keys.end(), rng);
        for (uint32_t i = 0; i < N - init_col_count * init_row_count; i++) {
            sketch.Delete(keys[i]);
            expected_sketch.Delete(keys[i]);
            if (i % check_period == 0) {
                sketch.FlushPrefetchQueue();
                REQUIRE_EQ(sketch.counter_count, expected_sketch.counter_count);
                REQUIRE_EQ(sketch.col_count, expected_sketch.col_count);
                REQUIRE_EQ(sketch.row_count, expected_sketch.row_count);
                REQUIRE_EQ(sketch.n, expected_sketch.n);
                REQUIRE_EQ(sketch.expansion_lim, expected_sketch.expansion_lim);
                REQUIRE_EQ(sketch.contraction_lim, expected_sketch.contraction_lim);
                for (uint32_t j = 0; j < sketch.sketches.size(); j++) {
                    for (uint32_t k = 0; k < sketch.sketches[j]->counter_count; k++)
                        REQUIRE_EQ(sketch.get_counter(sketch.sketches[j], k),
                                reinterpret_cast<const int32_t *>(expected_sketch.sketches[j])[k]);
                }
            }
        }
    }

private:
    static void PrintSketch(SublimeCS<> &s) {
        SublimeCS<>::Sketch *sketch = s.sketches.back();
        const uint32_t cache_line_count = (sketch->row_count * sketch->col_count + sketch->counter_per_cache_line - 1)
                                          / sketch->counter_per_cache_line;
        const uint32_t sep_1 = sketch->counter_per_cache_line;
        const uint32_t sep_2 = sep_1 + sketch->counter_per_cache_line * sketch->stub_size;
        const uint32_t sep_3 = sep_2 + sketch->last_extension_word_bit_count;
        std::cerr << "cache_line_count=" << cache_line_count << std::endl;
        for (int i = 0; i < cache_line_count; i++) {
            uint32_t cnt = 0;
            const uint8_t *ptr = sketch->sketch + i * SublimeCS<>::cache_line_size_bytes;
            for (int j = 0; j < SublimeCS<>::cache_line_size_bytes; j++)
                for (int k = 0; k < 8; k++) {
                    if (cnt == sep_1)
                        std::cerr << " --- ";
                    else if (cnt == sep_2) {
                        std::cerr << " --- ";
                    }
                    else if (cnt >= sep_3 && cnt % 64 == 63) {
                        std::cerr << ' ';
                    }
                    else if ((sep_1 < cnt && cnt < sep_2) && (cnt - sep_1) % sketch->stub_size == 0) {
                        std::cerr << ',';
                    }
                    std::cerr << ((ptr[j] >> k) & 1);
                    cnt++;
                }
            const uint64_t *words = reinterpret_cast<const uint64_t *>(ptr);
            if (s.has_tails_array(sketch, words)) {
                std::cerr << " ==== ";
                const uint32_t *inner_ptr = reinterpret_cast<uint32_t *>(words[SublimeCS<>::cache_line_size_words - 1]);
                for (int j = 0; j < sketch->counter_per_cache_line; j++)
                    std::cerr << inner_ptr[j] << ' ';
            }
            std::cerr << std::endl;
        }
        std::cerr << "row_count=" << s.row_count << " col_count=" << s.col_count << " -- init_col_count=" << s.init_col_count << std::endl;
        std::cerr << "contraction_lim=" << s.contraction_lim << " expansion_lim=" << s.expansion_lim << std::endl;
        std::cerr << "=======================================" << std::endl;
    }
};

TEST_SUITE("SublimeCS") {
    TEST_CASE("counter read/write") {
        SublimeCSTest::CounterRW1();
        SublimeCSTest::CounterRW2();
    }

    TEST_CASE("counter increment") {
        SublimeCSTest::CounterIncrement1();
        SublimeCSTest::CounterIncrement2();
    }

    TEST_CASE("counter decrement") {
        SublimeCSTest::CounterDecrement();
    }

    TEST_CASE("simple expansion and contraction") {
        SublimeCSTest::ExpandAndContract();
    }

    TEST_CASE("reallocate") {
        SublimeCSTest::Reallocate();
    }

    TEST_CASE("monte carlo") {
        SublimeCSTest::MonteCarlo();
    }
}

