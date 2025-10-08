#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "../bench_template.hpp"
#include "include/CodingCM.hpp"

inline BIT_CM_ver2 *init_sketch(const uint32_t memory_budget, const uint32_t row_count) {
    const uint32_t counter_count = memory_budget;
    const uint32_t seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count();
    BIT_CM_ver2 *sketch = new BIT_CM_ver2(counter_count, row_count, seed);
    return sketch;
}

inline void insert_sketch(BIT_CM_ver2 *sketch, const std::string& key) {
    char key_copy[key.size() + 1];
    memcpy(key_copy, key.c_str(), key.size());
    key_copy[key.size()] = 0;
    sketch->Insert(key_copy, key.size());
}

template<typename T>
inline void insert_sketch(BIT_CM_ver2 *sketch, T key) {
    char key_copy[sizeof(key) + 1];
    memcpy(key_copy, &key, sizeof(key));
    key_copy[sizeof(key)] = 0;
    sketch->Insert(key_copy, sizeof(key));
}

template<typename T>
inline void delete_sketch(BIT_CM_ver2 *sketch, const std::string&  key) {
    throw std::runtime_error("Deletes not implemented");
}

template<typename T>
inline void delete_sketch(BIT_CM_ver2 *sketch, T key) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(BIT_CM_ver2 *sketch, const std::string& key) {
    char key_copy[key.size() + 1];
    memcpy(key_copy, key.c_str(), key.size());
    key_copy[key.size()] = 0;
    return sketch->Query(key_copy, key.size());
}

template<typename T>
inline int32_t query_sketch(BIT_CM_ver2 *sketch, T key) {
    char key_copy[sizeof(key) + 1];
    memcpy(key_copy, &key, sizeof(key));
    key_copy[sizeof(key)] = 0;
    return sketch->Query(key_copy, sizeof(key));
}

inline uint32_t size_of_sketch(BIT_CM_ver2 *sketch) {
    // Uses parameters and expressions that seem to be hard-coded into the implementation...
    uint32_t res = sketch->layer[0].Len() * 10 / 8 + 8 + sketch->layer[0].Len() / 8 + 8;
    res += sketch->layer[1].Len() * 3 / 8 + 8 + sketch->layer[1].Len() / 8 + 8;
    res += sketch->layer[2].Len() * 3 / 8 + 8 + sketch->layer[2].Len() / 8 + 8;
    res += sketch->layer[3].Len() * 2 / 8 + 8 + sketch->layer[3].Len() / 8 + 8;
    res += sketch->layer[4].Len() * 2 / 8 + 8 + sketch->layer[4].Len() / 8 + 8;
    res += sketch->layer[5].Len() * 10 / 8 + 8 + sketch->layer[5].Len() / 8 + 8;
    res += sketch->layer[0].Len() * sizeof(int);
    return res;
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-CodingCM");

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

