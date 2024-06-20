#include <cstdint>

#include "../bench_template.hpp"
#include "CMSketchbookAdaptiveCounters.hpp"

inline CMSketchbookAdaptiveCounters init_sketch(const uint32_t memory_budget,
                                                const uint32_t row_count)
{
    const uint32_t counter_count = (8 * memory_budget + 8) / 9;
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    auto f = [](uint64_t x) { return x * x; };
    const uint32_t seed = 1380;
    CMSketchbookAdaptiveCounters sketch(col_count, row_count, f, seed);
    return sketch;
}

inline void insert_sketch(CMSketchbookAdaptiveCounters &sketch, const std::string &key)
{
    return sketch.Insert(key.c_str(), key.size());
}

inline uint32_t query_sketch(CMSketchbookAdaptiveCounters &sketch, const std::string &key)
{
    return sketch.Query(key.c_str(), key.size());
}

inline uint32_t size_of_sketch(CMSketchbookAdaptiveCounters &sketch)
{
    return 0;
}


int main(int argc, char const *argv[]) 
{
    auto parser = init_parser("bench-CMSketchbookAdaptiveCounters");

    try {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }

    auto insert_fun = [](auto &f, const std::string &key) { insert_sketch(f, key); };
    auto query_fun = [](auto &f, const std::string &key) { return query_sketch(f, key); };
    auto size_fun = [](auto &f) { return size_of_sketch(f); };

    auto [ keys, memory, rows ] = read_parser_arguments(parser);
    auto sketch = init_sketch(memory, rows);
    experiment(sketch, insert_fun, query_fun, size_fun, keys);
    print_test();
}

