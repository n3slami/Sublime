#include <cmath>
#include <cstdint>
#include <functional>

#include "../bench_template.hpp"
#include "CSketchbookFixedCounters.hpp"

template<typename T>
inline CSketchbookFixedCounters<T> *init_sketch(const uint32_t memory_budget,
                                                const uint32_t row_count,
                                                std::function<uint64_t(size_t)> f) {
    const uint32_t counter_count = (memory_budget + sizeof(T) - 1) / sizeof(T);
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    top_aae_are_count = col_count;  // No. of top items to compute AAE and ARE for 
    const uint32_t seed = 1380;
    CSketchbookFixedCounters<T> *sketch = new CSketchbookFixedCounters<T>(col_count, row_count, f, seed);
    return sketch;
}

template<typename T>
inline void insert_sketch(CSketchbookFixedCounters<T> *sketch, const std::string& key) {
    sketch->Insert(key.c_str(), key.size());
}

template<typename T, typename K>
inline void insert_sketch(CSketchbookFixedCounters<T> *sketch, K key) {
    sketch->Insert(key);
}

template<typename T>
inline void delete_sketch(CSketchbookFixedCounters<T> *sketch, const std::string& key) {
    sketch->Delete(key.c_str(), key.size());
}

template<typename T, typename K>
inline void delete_sketch(CSketchbookFixedCounters<T> *sketch, K key) {
    sketch->Delete(key);
}

template<typename T>
inline int32_t query_sketch(CSketchbookFixedCounters<T> *sketch, const std::string& key) {
    return sketch->Query(key.c_str(), key.size());
}

template<typename T, typename K>
inline int32_t query_sketch(CSketchbookFixedCounters<T> *sketch, K key) {
    return sketch->Query(key);
}

template<typename T>
inline uint32_t size_of_sketch(CSketchbookFixedCounters<T> *sketch) {
    return sketch->Size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-CSketchbookFixedCounters");

    try {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }

    memory_budget = parser.get<uint64_t>("arg");
    read_workload(parser.get<std::string>("--workload"));

    const uint32_t n_rows = parser.get<uint32_t>("--rows");
    const double expansion_power = parser.get<double>("--expansion-power");
    auto f = [&](size_t x) { return expansion_power == 0.0 ? std::numeric_limits<uint64_t>::max()
                                    : static_cast<uint64_t>(pow(x, 1.0 / expansion_power)); };
    auto sketch = init_sketch<uint64_t>(memory_budget, n_rows, f);
    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
    else 
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
}

