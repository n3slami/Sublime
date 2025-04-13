#include <cstdint>

#include "../bench_template.hpp"
#include "CMSketchbookAdaptiveCountersPQ.hpp"

#define TOF

inline CMSketchbookAdaptiveCountersPQ init_sketch(const uint32_t memory_budget,
                                                const uint32_t row_count)
{
    const uint32_t counter_count = memory_budget * (63.0 / 64.0);
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    auto f = [](uint64_t x) { return x * x; };
    const uint32_t seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count();
    CMSketchbookAdaptiveCountersPQ sketch(col_count, row_count, f, seed);
    return sketch;
}

int cnt = 0;

inline void insert_sketch(CMSketchbookAdaptiveCountersPQ &sketch, const std::string &key)
{
#ifdef TOF
    sketch.InsertTofHashing(key.c_str(), key.size());
#else
    sketch.Insert(key.c_str(), key.size());
#endif
}

inline uint32_t query_sketch(CMSketchbookAdaptiveCountersPQ &sketch, const std::string &key)
{
#ifdef TOF
    return sketch.QueryTofHashing(key.c_str(), key.size());
#else
    return sketch.Query(key.c_str(), key.size());
#endif
}

inline uint32_t size_of_sketch(CMSketchbookAdaptiveCountersPQ &sketch)
{
    return sketch.Size();
}


int main(int argc, char const *argv[]) 
{
    auto parser = init_parser("bench-CMSketchbookAdaptiveCountersPQ");

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

