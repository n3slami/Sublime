#include <cstdint>
#include <functional>

#include "../bench_template.hpp"
#include "CMSketchbookAdaptiveCountersPQ.hpp"

#define TOF

inline CMSketchbookAdaptiveCountersPQ *init_sketch(const uint32_t memory_budget,
                                                   const uint32_t row_count,
                                                   std::function<uint64_t(size_t)> f) {
    const uint32_t counter_count = memory_budget * (63.0 / 64.0);
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    const uint32_t seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count();
    CMSketchbookAdaptiveCountersPQ *sketch = new CMSketchbookAdaptiveCountersPQ(col_count, row_count, f, seed);
    return sketch;
}

int cnt = 0;

inline void insert_sketch(CMSketchbookAdaptiveCountersPQ *sketch, const std::string& key) {
#ifdef TOF
    sketch->InsertTofHashing(key.c_str(), key.size());
#else
    sketch->Insert(key.c_str(), key.size());
#endif
}

template<typename T>
inline void insert_sketch(CMSketchbookAdaptiveCountersPQ *sketch, T key) {
#ifdef TOF
    sketch->InsertTofHashing(reinterpret_cast<char *>(&key), sizeof(key));
#else
    sketch->Insert(key);
#endif
}

inline void delete_sketch(CMSketchbookAdaptiveCountersPQ *sketch, const std::string& key) {
#ifdef TOF
    sketch->DeleteTofHashing(key.c_str(), key.size());
#else
    sketch->Delete(key.c_str(), key.size());
#endif
}

template<typename T>
inline void delete_sketch(CMSketchbookAdaptiveCountersPQ *sketch, T key) {
#ifdef TOF
    sketch->DeleteTofHashing(reinterpret_cast<char *>(&key), sizeof(key));
#else
    sketch->Delete(key);
#endif
}

inline int32_t query_sketch(CMSketchbookAdaptiveCountersPQ *sketch, const std::string& key) {
#ifdef TOF
    return sketch->QueryTofHashing(key.c_str(), key.size());
#else
    return sketch->Query(key.c_str(), key.size());
#endif
}

template<typename T>
inline int32_t query_sketch(CMSketchbookAdaptiveCountersPQ *sketch, T key) {
#ifdef TOF
    return sketch->QueryTofHashing(reinterpret_cast<char *>(&key), sizeof(key));
#else
    return sketch->Query(key);
#endif
}

inline uint32_t size_of_sketch(CMSketchbookAdaptiveCountersPQ *sketch) {
    return sketch->Size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-CMSketchbookAdaptiveCountersPQ");

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
    auto sketch = init_sketch(memory_budget, n_rows, f);
    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
    else 
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
}

