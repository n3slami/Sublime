#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "../bench_template.hpp"
#include "include/Tailored.hpp"

inline Tailored *init_sketch(const uint32_t memory_budget, const uint32_t d) {
    Tailored *sketch = new Tailored(memory_budget, d);
    return sketch;
}

inline void insert_sketch(Tailored *sketch, const std::string& key) {
    sketch->Insert(key.c_str(), key.size());
}

template<typename T>
inline void insert_sketch(Tailored *sketch, T key) {
    char key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    sketch->Insert(key_str, sizeof(key));
}

template<typename T>
inline void delete_sketch(Tailored *sketch, const std::string& key) {
    throw std::runtime_error("Deletes not implemented");
}

template<typename T>
inline void delete_sketch(Tailored *sketch, T key) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(Tailored *sketch, const std::string& key) {
    return sketch->Query(key.c_str(), key.size());
}

template<typename T>
inline int32_t query_sketch(Tailored *sketch, T key) {
    char key_str[sizeof(key) + 1];
    memcpy(key_str, &key, sizeof(key));
    key_str[sizeof(key)] = 0;
    return sketch->Query(key_str, sizeof(key));
}

inline uint32_t size_of_sketch(Tailored *sketch) {
    return sketch->Size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-Tailored");

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

