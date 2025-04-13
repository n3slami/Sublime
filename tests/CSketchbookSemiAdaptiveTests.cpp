#include <cstdint>
#include <iostream>
#include <assert.h>
#include <string>

#include "CSketchbookSemiAdaptiveCounters.hpp"

const std::string ansi_green = "\033[0;32m";
const std::string ansi_white = "\033[0;97m";

class CSketchbookSemiAdaptiveCountersTest {
public:
    static void ExpandAndContract() {
        auto f = [](uint64_t x) { return x * x; };
        CSketchbookSemiAdaptiveCounters sketch(10, 10, f, 1);

        const uint32_t one_count = 100;
        for (int i = 0; i < one_count; i++)
            sketch.Insert(1);
        const uint64_t sign_hash = sketch.get_sign_hash(1);
        assert(sketch.Query(1) == one_count);
        assert(sketch.Query(2) == 0);
        int pos = -1;
        for (int i = 0; i < sketch.col_count; i++) {
            if (sketch.get_counter(sketch.sketches.back(), i) != 0) {
                pos = i;
                assert(sketch.get_counter(sketch.sketches.back(), pos) == 100 * ((sign_hash & 1) ? 1 : -1));
            }
            else
                assert(sketch.get_counter(sketch.sketches.back(), i) == 0);
        }
        assert(pos != -1);
        sketch.Insert(1);
        assert(sketch.row_count == 10 && sketch.col_count == 20);
        assert(sketch.get_counter(sketch.sketches.back(), pos) == 101 * ((sign_hash & 1) ? 1 : -1)
                && sketch.get_counter(sketch.sketches.back(), 10 + pos) == 100 * ((sign_hash & 1) ? 1 : -1));
        sketch.Delete(1);
        assert(sketch.row_count == 10 && sketch.col_count == 10);
        assert(sketch.get_counter(sketch.sketches.back(), pos) == 100 * ((sign_hash & 1) ? 1 : -1));
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

int main(int argc, char **argv) {
    std::cerr << ansi_green << "===== [ Running Tests ]" << ansi_white << std::endl;
    CSketchbookSemiAdaptiveCountersTest::ExpandAndContract();
    std::cerr << ansi_green << "===== [ Done ]" << ansi_white << std::endl;

    return 0;
}

