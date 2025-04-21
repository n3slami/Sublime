#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>
#include <assert.h>

#include "CSketchbookSemiAdaptiveCounters.hpp"

class CSketchbookSemiAdaptiveCountersTest {
public:
    static void ExpandAndContract() {
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookSemiAdaptiveCounters sketch(10, 10, f, 1);

        const uint32_t one_count = 100;
        for (int i = 0; i < one_count; i++)
            sketch.Insert(1);
        const uint64_t sign_hash = sketch.get_sign_hash(1);
        REQUIRE_EQ(sketch.Query(1), one_count);
        REQUIRE_EQ(sketch.Query(2), 0);

        int pos = -1;
        for (int i = 0; i < sketch.col_count; i++) {
            if (sketch.get_counter(sketch.sketches.back(), i) != 0) {
                pos = i;
                REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), pos), 100 * ((sign_hash & 1) ? 1 : -1));
            }
            else
                REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), i), 0);
        }
        REQUIRE_NE(pos, -1);

        sketch.Insert(1);
        REQUIRE_EQ(sketch.row_count, 10);
        REQUIRE_EQ(sketch.col_count, 20);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), pos), 101 * ((sign_hash & 1) ? 1 : -1));
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), 10 + pos), 100 * ((sign_hash & 1) ? 1 : -1));

        sketch.Delete(1);
        REQUIRE_EQ(sketch.row_count, 10);
        REQUIRE_EQ(sketch.col_count, 10);
        REQUIRE_EQ(sketch.get_counter(sketch.sketches.back(), pos), 100 * ((sign_hash & 1) ? 1 : -1));
    }

private:
    static void PrintSketch(CSketchbookSemiAdaptiveCounters &sketchbook) {
        uint8_t *sketch = sketchbook.sketches.back();
        for (int i = 0; i < sketchbook.row_count; i++) {
            for (int j = 0; j < sketchbook.col_count; j++)
                std::cerr << sketchbook.get_counter(sketch, i * sketchbook.col_count + j) << ' ';
            std::cerr << std::endl;
        }
        std::cerr << "row_count=" << sketchbook.row_count << " col_count=" << sketchbook.col_count << " -- init_col_count=" << sketchbook.init_col_count << std::endl;
        std::cerr << "contraction_lim=" << sketchbook.contraction_lim << " expansion_lim=" << sketchbook.expansion_lim << std::endl;
        std::cerr << "+++ counter_width=" << sketchbook.counter_width << std::endl;
        std::cerr << "=======================================" << std::endl;
    }
};

TEST_SUITE("CSketchbookSemiAdaptiveCounters") {
    TEST_CASE("expand and contract") {
        CSketchbookSemiAdaptiveCountersTest::ExpandAndContract();
    }
}

