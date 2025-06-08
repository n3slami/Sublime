#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "../bench_template.hpp"
#include <cmsketch.h>
#include <params.h>

static uint32_t sketch_w, sketch_d;
char SmallActiveCounter::q = per_estimator_int;
char SmallActiveCounter::mode = per_estimator_mode;
char SmallActiveCounter::A = per_estimator_int - per_estimator_mode;
int SmallActiveCounter::r = 1;


inline CMSketch *init_sketch(const uint32_t memory_budget, const uint32_t row_count) {
    const uint32_t counter_count = memory_budget;
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    CMSketch *sketch = new CMSketch(col_count, row_count);
    sketch_w = col_count;
    sketch_d = row_count;
    return sketch;
}

inline void insert_sketch(CMSketch *sketch, const std::string& key) {
    sketch->dynamic_sead_insert(key.c_str(), 1, gamma_2);
}

template<typename T>
inline void insert_sketch(CMSketch *sketch, T key) {
    char key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    sketch->dynamic_sead_insert(key_str, 1, gamma_2);
}

template<typename T>
inline void delete_sketch(CMSketch *sketch, const std::string& key) {
    sketch->dynamic_sead_insert(key.c_str(), -1, gamma_2);
}

template<typename T>
inline void delete_sketch(CMSketch *sketch, T key) {
    char key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    sketch->dynamic_sead_insert(key_str, -1, gamma_2);
}

inline int32_t query_sketch(CMSketch *sketch, const std::string& key) {
    return sketch->dynamic_sead_query(key.c_str(), gamma_2);
}

template<typename T>
inline int32_t query_sketch(CMSketch *sketch, T key) {
    char key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    return sketch->dynamic_sead_query(key_str, gamma_2);
}

inline uint32_t size_of_sketch(CMSketch *sketch) {
    return sketch_w * sketch_d * sizeof(sead_c);
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-SEADCM");

    try {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }

    memory_budget = parser.get<uint64_t>("arg");
    read_workload(parser.get<std::string>("--workload"));

    const uint32_t n_rows = parser.get<uint32_t>("--rows");
    auto sketch = init_sketch(memory_budget, n_rows);
    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
    else 
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
}

