#include <cstdint>
#include <functional>

#include "../bench_template.hpp"
#include "Waving.h"

static constexpr uint32_t num_slots = 8;
static constexpr uint32_t num_counters = 1;
typedef uint32_t fingerprint_t;
static constexpr uint32_t data_len = sizeof(fingerprint_t);
static uint32_t key_hash_seed;

inline WavingSketch<num_slots, num_counters, data_len> *init_sketch(const uint32_t memory_budget) {
    const uint32_t memory_per_bucket = num_slots * data_len + num_slots * sizeof(int) + num_counters * sizeof(int16_t);
    key_hash_seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count();
    return new WavingSketch<num_slots, num_counters, data_len>(memory_budget / memory_per_bucket);
}

inline void insert_sketch(WavingSketch<num_slots, num_counters, data_len> *sketch, const std::string& key) {
    const uint32_t key_hash = MurmurHash3_x86_32(key.c_str(), key.size(), key_hash_seed);
    char key_hash_str[data_len + 1];
    memcpy(key_hash_str, &key_hash, sizeof(key_hash));
    key_hash_str[sizeof(key_hash)] = 0;
    sketch->Init(Data<data_len>(key_hash_str));
}

template <typename T>
inline void insert_sketch(WavingSketch<num_slots, num_counters, data_len> *sketch, T key) {
    const uint32_t key_hash = MurmurHash3_x86_32(&key, sizeof(key), key_hash_seed);
    char key_hash_str[data_len + 2];
    memcpy(key_hash_str, &key_hash, sizeof(key_hash));
    key_hash_str[sizeof(key_hash)] = 0;
    sketch->Init(Data<data_len>(key_hash_str));
}

inline void delete_sketch(WavingSketch<num_slots, num_counters, data_len> *sketch, const std::string& key) {
    throw std::runtime_error("Deletes not implemented");
}

template <typename T>
inline void delete_sketch(WavingSketch<num_slots, num_counters, data_len> *sketch, T key) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(WavingSketch<num_slots, num_counters, data_len> *sketch, const std::string& key) {
    const uint32_t key_hash = MurmurHash3_x86_32(key.c_str(), key.size(), key_hash_seed);
    char key_hash_str[data_len + 1];
    memcpy(key_hash_str, &key_hash, sizeof(key_hash));
    key_hash_str[sizeof(key_hash)] = 0;
    return sketch->Query(Data<data_len>(key_hash_str));
}

template <typename T>
inline int32_t query_sketch(WavingSketch<num_slots, num_counters, data_len> *sketch, T key) {
    const uint32_t key_hash = MurmurHash3_x86_32(&key, sizeof(key), key_hash_seed);
    char key_hash_str[data_len + 1];
    memcpy(key_hash_str, &key_hash, sizeof(key_hash));
    key_hash_str[sizeof(key_hash)] = 0;
    return sketch->Query(Data<data_len>(key_hash_str));
}

inline uint32_t size_of_sketch(WavingSketch<num_slots, num_counters, data_len> *sketch) {
    return sketch->getMemSize();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-Waving");

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

    auto sketch = init_sketch(memory_budget);
    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
    else 
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
}

