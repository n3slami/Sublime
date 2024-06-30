#include <cstdint>
#include <iostream>
#include <assert.h>
#include <string>

#include "CSketchbookFixedCounters.hpp"

const std::string ansi_green = "\033[0;32m";
const std::string ansi_white = "\033[0;97m";

class CSketchbookFixedCountersTest {
public:
    static void ExpandAndContract() {
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookFixedCounters<int64_t> sketch(10, 10, f, 1);

        const uint32_t one_count = 100;
        for (int i = 0; i < one_count; i++)
            sketch.Insert(1);
        assert(sketch.Query(1) == one_count);
        assert(sketch.Query(2) == 0);

        int pos = -1;
        const uint64_t sign_hash = sketch.get_sign_hash(1);
        for (int i = 0; i < sketch.col_count; i++) {
            if (get_counter(sketch, i) != 0) {
                pos = i;
                assert(get_counter(sketch, pos) == (sign_hash & 1ULL) ? 100 : -100);
            }
            else 
                assert(get_counter(sketch, i) == 0);
        }
        assert(pos != -1);
        sketch.Insert(1);
        assert(sketch.row_count == 10 && sketch.col_count == 20);
        assert(get_counter(sketch, pos) == ((sign_hash & 1ULL) ? 101 : -101)
                && get_counter(sketch, 10 + pos) == ((sign_hash & 1ULL) ? 100 : -100));
        sketch.Delete(1);
        assert(sketch.row_count == 10 && sketch.col_count == 10);
        assert(get_counter(sketch, pos) == ((sign_hash & 1ULL) ? 100 : -100));
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

int main(int argc, char **argv) {
    std::cerr << ansi_green << "===== [ Running Tests ]" << ansi_white << std::endl;
    CSketchbookFixedCountersTest::ExpandAndContract();
    std::cerr << ansi_green << "===== [ Done ]" << ansi_white << std::endl;

    return 0;
}
