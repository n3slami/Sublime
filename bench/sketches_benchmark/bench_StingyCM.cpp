#include <cstdint>
#include <stdexcept>

#include "../bench_template.hpp"
#include "include/StingyCM.hpp"

inline StingyCM *init_sketch(const uint32_t memory_budget, const uint32_t row_count) {
    const uint32_t counter_count = memory_budget;
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    auto f = [](uint64_t x) { return x * x; };
    const uint32_t seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count();
    StingyCM *sketch = new StingyCM(col_count * row_count, row_count, seed);
    return sketch;
}

inline void insert_sketch(StingyCM *sketch, const std::string& key) {
    sketch->Insert(key.c_str(), key.size());
}

template<typename T>
inline void insert_sketch(StingyCM *sketch, T key) {
    sketch->Insert(reinterpret_cast<char *>(&key), sizeof(key));
}

template<typename T>
inline void delete_sketch(StingyCM *sketch, const std::string&  key) {
    throw std::runtime_error("Deletes not implemented");
}

template<typename T>
inline void delete_sketch(StingyCM *sketch, T key) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(StingyCM *sketch, const std::string& key) {
    return sketch->Query(key.c_str(), key.size());
}

template<typename T>
inline int32_t query_sketch(StingyCM *sketch, T key) {
    return sketch->Query(reinterpret_cast<char *>(&key), sizeof(key));
}

inline uint32_t size_of_sketch(StingyCM *sketch) {
    return sketch->Size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-StingyCM");

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

