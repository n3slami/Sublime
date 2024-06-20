#include <cstdint>

#include "../bench_template.hpp"
#include "CMSketchbookFixedCounters.hpp"

template<typename T>
inline CMSketchbookFixedCounters<T> init_sketch(const uint32_t memory_budget,
                                                const uint32_t row_count)
{
    const uint32_t counter_count = (memory_budget + sizeof(T) - 1) / sizeof(T);
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    auto f = [](uint64_t x) { return x * x; };
    const uint32_t seed = 1380;
    CMSketchbookFixedCounters<T> sketch(col_count, row_count, f, seed);
    return sketch;
}

template<typename T>
inline void insert_sketch(CMSketchbookFixedCounters<T> &sketch, const std::string &key)
{
    return sketch.Insert(key.c_str(), key.size());
}

template<typename T>
inline uint32_t query_sketch(CMSketchbookFixedCounters<T> &sketch, const std::string &key)
{
    return sketch.Query(key.c_str(), key.size());
}

template<typename T>
inline uint32_t size_of_sketch(CMSketchbookFixedCounters<T> &sketch)
{
    return 0;
}


int main(int argc, char const *argv[]) 
{
    auto parser = init_parser("bench-CMSketchbookFixedCounters");

    try {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }

    auto insert_fun = [](auto &f, const std::string &key) { insert_sketch<uint32_t>(f, key); };
    auto query_fun = [](auto &f, const std::string &key) { return query_sketch<uint32_t>(f, key); };
    auto size_fun = [](auto &f) { return size_of_sketch<uint32_t>(f); };

    auto [ keys, memory, rows ] = read_parser_arguments(parser);
    auto sketch = init_sketch<uint32_t>(memory, rows);
    experiment(sketch, insert_fun, query_fun, size_fun, keys);
    print_test();
}

