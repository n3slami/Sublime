#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>
#include <assert.h>

#include "CSketchbookAdaptiveCounters.hpp"

class CSketchbookAdaptiveCountersTest {
public:
    static void CounterRW1() {
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookAdaptiveCounters sketch(10, 10, f, 1);

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
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookAdaptiveCounters sketch(10, 10, f, 1);

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
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookAdaptiveCounters sketch(10, 10, f, 1);

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
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookAdaptiveCounters sketch(10, 10, f, 1);

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
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookAdaptiveCounters sketch(10, 10, f, 1);

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
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookAdaptiveCounters sketch(10, 10, f, 1);

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
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookAdaptiveCounters sketch(10, 10, f, 1);

        /*
        const uint32_t one_count = 100;
        for (int i = 0; i < one_count; i++)
            sketch.Insert(1);
        assert(sketch.Query(1) == one_count);
        assert(sketch.Query(2) == 0);
        int pos = -1;
        for (int i = 0; i < sketch.col_count; i++) {
            if (sketch.sketches.back()[i] > 0) {
                pos = i;
                assert(sketch.sketches.back()[i] == 100);
            }
            else 
                assert(sketch.sketches.back()[pos] == 0);
        }
        assert(pos != -1);
        sketch.Insert(1);
        assert(sketch.row_count == 10 && sketch.col_count == 20);
        assert(sketch.sketches.back()[pos] == 101 && sketch.sketches.back()[10 + pos] == 100);
        sketch.Delete(1);
        assert(sketch.row_count == 10 && sketch.col_count == 10);
        assert(sketch.sketches.back()[pos] == 100);
        */
    }

private:
    static void PrintSketch(CSketchbookAdaptiveCounters &sketchbook) {
        uint8_t *sketch = sketchbook.sketches.back();
        const uint32_t cache_line_count = (sketchbook.row_count * sketchbook.col_count 
                                           + CSketchbookAdaptiveCounters::counter_per_cache_line - 1)
                                          / CSketchbookAdaptiveCounters::counter_per_cache_line;
        const uint32_t sep_1 = CSketchbookAdaptiveCounters::counter_per_cache_line;
        const uint32_t sep_2 = sep_1 + CSketchbookAdaptiveCounters::counter_per_cache_line 
                                        * CSketchbookAdaptiveCounters::base_counter_size;
        const uint32_t sep_3 = sep_2 + CSketchbookAdaptiveCounters::second_word_bit_count;
        std::cerr << "cache_line_count=" << cache_line_count << std::endl;
        for (int i = 0; i < cache_line_count; i++) {
            uint32_t cnt = 0;
            uint8_t *ptr = sketch + i * CSketchbookAdaptiveCounters::cache_line_size_bytes;
            for (int j = 0; j < CSketchbookAdaptiveCounters::cache_line_size_bytes; j++)
                for (int k = 0; k < 8; k++) {
                    if (cnt == sep_1)
                        std::cerr << " --- ";
                    else if (cnt == sep_2) {
                        std::cerr << " --- ";
                    }
                    else if (cnt == sep_3) {
                        std::cerr << ' ';
                    }
                    else if ((sep_1 < cnt && cnt < sep_2)
                            && (cnt - sep_1) % CSketchbookAdaptiveCounters::base_counter_size == 0) {
                        std::cerr << ',';
                    }
                    std::cerr << ((ptr[j] >> k) & 1);
                    cnt++;
                }
            uint64_t *words = reinterpret_cast<uint64_t *>(ptr);
            if (words[CSketchbookAdaptiveCounters::cache_line_size_words - 2] >> 63) {
                std::cerr << " ==== ";
                uint32_t *inner_ptr = reinterpret_cast<uint32_t *>(words[CSketchbookAdaptiveCounters::cache_line_size_words - 1]);
                for (int j = 0; j < CSketchbookAdaptiveCounters::counter_per_cache_line; j++)
                    std::cerr << inner_ptr[j] << ' ';
            }
            std::cerr << std::endl;
        }
        std::cerr << "row_count=" << sketchbook.row_count << " col_count=" << sketchbook.col_count << " -- init_col_count=" << sketchbook.init_col_count << std::endl;
        std::cerr << "contraction_lim=" << sketchbook.contraction_lim << " expansion_lim=" << sketchbook.expansion_lim << std::endl;
        std::cerr << "=======================================" << std::endl;
    }
};

TEST_SUITE("CSketchbookAdaptiveCounters") {
    TEST_CASE("counter read/write") {
        CSketchbookAdaptiveCountersTest::CounterRW1();
        CSketchbookAdaptiveCountersTest::CounterRW2();
    }

    TEST_CASE("counter increment") {
        CSketchbookAdaptiveCountersTest::CounterIncrement1();
        CSketchbookAdaptiveCountersTest::CounterIncrement2();
    }

    TEST_CASE("counter decrement") {
        CSketchbookAdaptiveCountersTest::CounterDecrement();
    }
}

