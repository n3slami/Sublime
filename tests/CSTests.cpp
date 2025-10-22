#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include <cstdint>
#include <iostream>
#include <assert.h>

#include "CS.hpp"

class CSTest {
public:
    static void ExpandAndContract() {
        const uint32_t init_col_count = 10;
        const uint32_t init_row_count = 10;
        auto f = [](double x) { return static_cast<uint64_t>(x * x); };
        CS<int64_t> sketch(init_col_count, init_row_count, f, 1);

        const int64_t key = 1;
        const uint32_t one_count = f(init_col_count);
        for (int i = 0; i < one_count; i++)
            sketch.Insert(key);
        REQUIRE_EQ(sketch.Query(key), one_count);
        REQUIRE_EQ(sketch.Query(key + 1), 0);

        int pos = -1;
        const uint64_t sign_hash = sketch.get_sign_hash(reinterpret_cast<const char *>(&key), sizeof(key));
        for (int i = 0; i < sketch.col_count; i++) {
            if (get_counter(sketch, i) != 0) {
                pos = i;
                REQUIRE_EQ(get_counter(sketch, pos), (sign_hash & 1ULL) ? 100 : -100);
            }
            else 
                REQUIRE_EQ(get_counter(sketch, i), 0);
        }
        REQUIRE_NE(pos, -1);
        sketch.Insert(key);
        REQUIRE_EQ(sketch.col_count, 2 * init_col_count);
        REQUIRE_EQ(sketch.row_count, init_row_count);
        REQUIRE_EQ(get_counter(sketch, pos), ((sign_hash & 1ULL) ? 101 : -101));
        REQUIRE_EQ(get_counter(sketch, 10 + pos), ((sign_hash & 1ULL) ? 100 : -100));

        sketch.Delete(key);
        sketch.Delete(key);
        assert(sketch.row_count == 10 && sketch.col_count == 10);
        REQUIRE_EQ(sketch.col_count, init_col_count);
        REQUIRE_EQ(sketch.row_count, init_row_count);
        REQUIRE_EQ(get_counter(sketch, pos), ((sign_hash & 1ULL) ? 99 : -99));
    }

private:
    template<typename T>
    static T get_counter(CS<T> &sketchbook, uint32_t i) {
        return reinterpret_cast<T *>(sketchbook.sketches.back())[i];
    }

    template<typename T>
    static void PrintSketch(CS<T> &sketchbook) {
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
        CSTest::ExpandAndContract();
    }
}

