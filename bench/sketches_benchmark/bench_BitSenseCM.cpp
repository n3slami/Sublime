#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "../bench_template.hpp"
#include <sketch/BSCMSketch.h>
#include <common/bitsense.h>
#include <common/flowkey.h>

constexpr uint32_t key_len = 16;
constexpr uint32_t no_layer = 2;
typedef OmniSketch::Sketch::BSCMSketch<key_len, no_layer, int32_t, OmniSketch::Hash::AwareHash> BSCMSketchSpec;

inline BSCMSketchSpec *init_sketch(const uint32_t memory_budget, const uint32_t row_count) {
    const uint32_t counter_count = memory_budget;
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    constexpr double cnt_no_ratio = 0.1;
    std::vector<size_t> width_cnt = {5, 14};
    std::vector<size_t> no_hash = {3};
    BSCMSketchSpec *sketch = new BSCMSketchSpec(col_count, row_count,
                                                cnt_no_ratio, width_cnt, no_hash);
    return sketch;
}

inline void insert_sketch(BSCMSketchSpec *sketch, const std::string& key) {
    OmniSketch::FlowKey<key_len> flow_key(reinterpret_cast<const int8_t *>(key.c_str()));
    sketch->update(flow_key, 1);
}

template<typename T>
inline void insert_sketch(BSCMSketchSpec *sketch, T key) {
    int8_t key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    OmniSketch::FlowKey<key_len> flow_key(key_str);
    sketch->update(flow_key, 1);
}

template<typename T>
inline void delete_sketch(BSCMSketchSpec *sketch, const std::string&  key) {
    OmniSketch::FlowKey<key_len> flow_key(reinterpret_cast<const int8_t *>(key.c_str()));
    sketch->update(flow_key, -1);
}

template<typename T>
inline void delete_sketch(BSCMSketchSpec *sketch, T key) {
    int8_t key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    OmniSketch::FlowKey<key_len> flow_key(key_str);
    sketch->update(flow_key, -1);
}

inline int32_t query_sketch(BSCMSketchSpec *sketch, const std::string& key) {
    OmniSketch::FlowKey<key_len> flow_key(reinterpret_cast<const int8_t *>(key.c_str()));
    return sketch->query(flow_key);
}

template<typename T>
inline int32_t query_sketch(BSCMSketchSpec *sketch, T key) {
    int8_t key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    OmniSketch::FlowKey<key_len> flow_key(key_str);
    return sketch->query(flow_key);
}

inline uint32_t size_of_sketch(BSCMSketchSpec *sketch) {
    return sketch->size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-BitSenseCM");

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

