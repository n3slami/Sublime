#include <cstdint>
#include <functional>
#include <unordered_map>

#include "../bench_template.hpp"
#define RETUNE_SPILL_FRAC 1.00
#include "CMSketchbookAdaptiveCountersPQ.hpp"


inline CMSketchbookAdaptiveCountersPQ *init_sketch(const uint32_t memory_budget,
                                                   const uint32_t row_count,
                                                   std::function<uint64_t(size_t)> f) {
    const uint32_t counter_count = memory_budget / (static_cast<float>(CMSketchbookAdaptiveCountersPQ::cache_line_size_bytes) 
                                                    / CMSketchbookAdaptiveCountersPQ::default_counter_per_cache_line);
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    const uint32_t seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count();
    CMSketchbookAdaptiveCountersPQ *sketch = new CMSketchbookAdaptiveCountersPQ(col_count, row_count, f, seed);
    return sketch;
}

inline void insert_sketch(CMSketchbookAdaptiveCountersPQ *sketch, const std::string& key) {
    sketch->Insert(key.c_str(), key.size());
}

template <typename T>
inline void insert_sketch(CMSketchbookAdaptiveCountersPQ *sketch, T key) {
    sketch->Insert(key);
}

inline void delete_sketch(CMSketchbookAdaptiveCountersPQ *sketch, const std::string& key) {
    sketch->Delete(key.c_str(), key.size());
}

template <typename T>
inline void delete_sketch(CMSketchbookAdaptiveCountersPQ *sketch, T key) {
    sketch->Delete(key);
}

inline int32_t query_sketch(CMSketchbookAdaptiveCountersPQ *sketch, const std::string& key) {
    return sketch->Query(key.c_str(), key.size());
}

template <typename T>
inline int32_t query_sketch(CMSketchbookAdaptiveCountersPQ *sketch, T key) {
    return sketch->Query(key);
}

inline uint32_t size_of_sketch(CMSketchbookAdaptiveCountersPQ *sketch) {
    return sketch->Size();
}

inline std::unordered_map<std::string, uint32_t> get_vale_parameters(CMSketchbookAdaptiveCountersPQ *sketch) {
    std::unordered_map<std::string, uint32_t> res;
    res["counters_per_chunk"] = sketch->GetCountersPerChunk();
    res["stub_length"] = sketch->GetStubLength();
    return res;
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-CMSketchbookAdaptiveCountersPQNoTuning");

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
    const double size_function_power = parser.get<double>("--size-function-power");
    const double size_function_mult = parser.get<double>("--size-function-mult");
    auto f = [&](size_t x) { return size_function_power == 0.0 ? std::numeric_limits<uint64_t>::max()
                                    : static_cast<uint64_t>(pow(x, 1.0 / size_function_power) * size_function_mult); };
    auto sketch = init_sketch(memory_budget, n_rows, f);
    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch), 
                          reinterpret_cast<void *>(get_vale_parameters));
    else 
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch), 
                   reinterpret_cast<void *>(get_vale_parameters));
}

