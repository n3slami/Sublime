#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "../bench_template.hpp"
#include "SalsaCMS.hpp"

constexpr int32_t downsamplings_per_merge = 0;
constexpr uint32_t col_count_div = 16;
static uint32_t memory;

inline SalsaCMSSanity *init_sketch(const uint32_t memory_budget, const uint32_t row_count) {
    const uint32_t counter_count = memory_budget * 8 / 9;
    uint32_t col_count = (counter_count + row_count - 1) / row_count;
    if (__builtin_popcount(col_count) != 1) {
        const uint32_t orig_col_count = col_count;
        for (col_count = 1; col_count < orig_col_count; col_count <<= 1);
        col_count >>= 1;
    }
    memory = counter_count * 9 / 8;
    const uint32_t seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count() % 128;
    SalsaCMSSanity *sketch = new SalsaCMSSanity(col_count, row_count, seed, downsamplings_per_merge);
    return sketch;
}

inline void insert_sketch(SalsaCMSSanity *sketch, const std::string& key) {
    sketch->increment(key.c_str());
}

template<typename T>
inline void insert_sketch(SalsaCMSSanity *sketch, T key) {
    char key_str[sizeof(key) + 1] = {0};
    memcpy(key_str, &key, sizeof(key));
    sketch->increment(key_str);
}

template<typename T>
inline void delete_sketch(SalsaCMSSanity *sketch, const std::string&  key) {
    throw std::runtime_error("Deletes not implemented");
}

template<typename T>
inline void delete_sketch(SalsaCMSSanity *sketch, T key) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(SalsaCMSSanity *sketch, const std::string& key) {
    return sketch->query(key.c_str());
}

template<typename T>
inline int32_t query_sketch(SalsaCMSSanity *sketch, T key) {
    char key_str[sizeof(key) + 1] = {0};
    memcpy(key_str, &key, sizeof(key));
    return sketch->query(key_str);
}

inline uint32_t size_of_sketch(void *sketch) {
    return memory;
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-SALSACM");

    try {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }

    auto memory_budgets = parser.get<std::vector<uint64_t>>("arg");
    read_workload(parser.get<std::string>("--workload"));

    const uint32_t n_rows = parser.get<uint32_t>("--rows");

    if (memory_budgets.size() == 1) {
        auto sketch = init_sketch(memory_budgets[0], n_rows);
        if (wio.StringKeys()) {
            experiment_string(sketch, pass_fun(insert_sketch),
                    pass_fun(delete_sketch),
                    pass_fun(query_sketch),
                    pass_fun(size_of_sketch));
        }
        else {
            experiment(sketch, pass_fun(insert_sketch),
                    pass_fun(delete_sketch),
                    pass_fun(query_sketch),
                    pass_fun(size_of_sketch));
        }
    }
    else {
        std::vector sketches { init_sketch(memory_budgets[0], n_rows) };
        for (int32_t i = 1; i < memory_budgets.size(); i++)
            sketches.push_back(init_sketch(memory_budgets[i], n_rows));
        experiment_join_string(sketches,
                pass_fun(insert_sketch),
                pass_fun(delete_sketch),
                pass_fun(query_sketch),
                pass_fun(size_of_sketch));
    }
}

