#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>
#include <assert.h>

#include "CSketchbookFixedCounters.hpp"

class CSketchbookFixedCountersTest {
public:
    static void ExpandAndContract() {
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookFixedCounters<int64_t> sketch(10, 10, f, 1);

        const uint32_t one_count = 100;
        for (int i = 0; i < one_count; i++)
            sketch.Insert(1);
        REQUIRE_EQ(sketch.Query(1), one_count);
        REQUIRE_EQ(sketch.Query(2), 0);

        int pos = -1;
        const uint64_t sign_hash = sketch.get_sign_hash(1);
        for (int i = 0; i < sketch.col_count; i++) {
            if (get_counter(sketch, i) != 0) {
                pos = i;
                REQUIRE_EQ(get_counter(sketch, pos), (sign_hash & 1ULL) ? 100 : -100);
            }
            else 
                REQUIRE_EQ(get_counter(sketch, i), 0);
        }
        REQUIRE_NE(pos, -1);
        sketch.Insert(1);
        REQUIRE_EQ(sketch.row_count, 10);
        REQUIRE_EQ(sketch.col_count, 20);
        REQUIRE_EQ(get_counter(sketch, pos), ((sign_hash & 1ULL) ? 101 : -101));
        REQUIRE_EQ(get_counter(sketch, 10 + pos), ((sign_hash & 1ULL) ? 100 : -100));

        sketch.Delete(1);
        assert(sketch.row_count == 10 && sketch.col_count == 10);
        REQUIRE_EQ(sketch.row_count, 10);
        REQUIRE_EQ(sketch.col_count, 10);
        REQUIRE_EQ(get_counter(sketch, pos), ((sign_hash & 1ULL) ? 100 : -100));
    }

private:
    template<typename T>
    static T get_counter(CSketchbookFixedCounters<T> &sketchbook, uint32_t i) {
        return reinterpret_cast<T *>(sketchbook.sketches.back())[i];
    }

    template<typename T>
    static void PrintSketch(CSketchbookFixedCounters<T> &sketchbook) {
        T *sketch = (T*) sketchbook.sketches.back();
        for (int i = 0; i < sketchbook.row_count; i++) {
            for (int j = 0; j < sketchbook.col_count; j++)
                std::cerr << sketch[i * sketchbook.col_count + j] << ' ';
            std::cerr << std::endl;
        }
        std::cerr << "row_count=" << sketchbook.row_count << " col_count=" << sketchbook.col_count << " -- init_col_count=" << sketchbook.init_col_count << std::endl;
        std::cerr << "contraction_lim=" << sketchbook.contraction_lim << " expansion_lim=" << sketchbook.expansion_lim << std::endl;
        std::cerr << "=======================================" << std::endl;
    }
};


TEST_SUITE("CSketchbookFixedCounters") {
    TEST_CASE("expand and contract") {
        CSketchbookFixedCountersTest::ExpandAndContract();
    }
}

