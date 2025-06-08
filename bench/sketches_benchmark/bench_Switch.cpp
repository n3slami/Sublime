#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "../bench_template.hpp"
#include "include/Switch.hpp"

inline void *init_sketch(const uint32_t memory_budget, const uint32_t d) {
    const uint32_t bucket_num = memory_budget / sizeof(uint64_t) / d;
    allocate(bucket_num, d);
    keys[0] = new uint8_t[1000];
    keys[1] = new uint8_t[1000];
    return nullptr;
}

inline void insert_sketch(void *sketch, const std::string& key) {
    assert(sketch == nullptr);
    memcpy(keys[0], key.c_str(), key.size());
    keys[0][key.size()] = 0;
    insert(1);
}

template<typename T>
inline void insert_sketch(void *sketch, T key) {
    assert(sketch == nullptr);
    memcpy(keys[0], &key, sizeof(key));
    keys[0][sizeof(key)] = 0;
    insert(1);
}

template<typename T>
inline void delete_sketch(void *sketch, const std::string& key) {
    throw std::runtime_error("Deletes not implemented");
}

template<typename T>
inline void delete_sketch(void *sketch, T key) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(void *sketch, const std::string& key) {
    assert(sketch == nullptr);
    memcpy(keys[1], key.c_str(), key.size());
    keys[1][key.size()] = 0;
    return query(keys[1]);
}

template<typename T>
inline int32_t query_sketch(void *sketch, T key) {
    assert(sketch == nullptr);
    memcpy(keys[1], &key, sizeof(key));
    keys[1][sizeof(key)] = 0;
    return query(keys[1]);
}

inline uint32_t size_of_sketch(void *sketch) {
    assert(sketch == nullptr);
    return size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-Switch");

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

