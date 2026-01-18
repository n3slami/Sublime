#include <cmath>
#include <cstdint>
#include <functional>

#include "../bench_template.hpp"
#include "CMS.hpp"

template<typename T>
inline CMS<T> *init_sketch(const uint32_t memory_budget, const uint32_t row_count, std::function<uint64_t(size_t)> f) {
    const uint32_t counter_count = (memory_budget + sizeof(T) - 1) / sizeof(T);
    const uint32_t col_count = (counter_count + row_count - 1) / row_count;
    top_aae_are_count = col_count;  // No. of top items to compute AAE and ARE for 
    const uint32_t seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now()) \
                                .time_since_epoch().count();
    CMS<T> *sketch = new CMS<T>(col_count, row_count, f, seed);
    return sketch;
}

template<typename T>
inline void insert_sketch(CMS<T> *sketch, const std::string& key) {
    sketch->Insert(key.c_str(), key.size());
}

template<typename T, typename K>
inline void insert_sketch(CMS<T> *sketch, K key) {
    sketch->Insert(key);
}

template<typename T>
inline void delete_sketch(CMS<T> *sketch, const std::string& key) {
    sketch->Delete(key.c_str(), key.size());
}

template<typename T, typename K>
inline void delete_sketch(CMS<T> *sketch, K key) {
    sketch->Delete(key);
}

template<typename T>
inline int32_t query_sketch(CMS<T> *sketch, const std::string& key) {
    return sketch->Query(key.c_str(), key.size());
}

template<typename T, typename K>
inline int32_t query_sketch(CMS<T> *sketch, K key) {
    return sketch->Query(key);
}

template<typename T>
inline uint32_t size_of_sketch(CMS<T> *sketch) {
    return sketch->Size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-CMS");

    try {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }

    auto memory_budgets = parser.get<std::vector<uint64_t>>("arg");
    read_workload(parser.get<std::string>("--workload"));

    const uint32_t n_rows = parser.get<uint32_t>("--rows");
    const double size_function_power = parser.get<double>("--size-function-power");
    const double size_function_mult = parser.get<double>("--size-function-mult");
    auto f = [&](size_t x) { return size_function_power == 0.0 ? std::numeric_limits<uint64_t>::max()
                                    : static_cast<uint64_t>(pow(x, 1.0 / size_function_power) * size_function_mult); };

    if (memory_budgets.size() == 1) {
        auto sketch = init_sketch<int64_t>(memory_budgets[0], n_rows, f);
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
        std::vector sketches { init_sketch<int64_t>(memory_budgets[0], n_rows, f) };
        for (int32_t i = 1; i < memory_budgets.size(); i++)
            sketches.push_back(init_sketch<int64_t>(memory_budgets[i], n_rows, f));
        experiment_join_string(sketches,
                pass_fun(insert_sketch),
                pass_fun(delete_sketch),
                pass_fun(query_sketch),
                pass_fun(size_of_sketch));
    }
}

