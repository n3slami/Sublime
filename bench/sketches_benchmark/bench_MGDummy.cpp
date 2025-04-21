#include <cstdint>
#include <limits>
#include <stdexcept>
#include <sys/types.h>

#include "../bench_template.hpp"
#include "MGDummy.hpp"

template<typename K, typename T>
inline MGDummy<K, T> *init_sketch(const uint32_t memory_budget, std::function<uint64_t(size_t)> f) {
    const float load_factor = 0.95;
    const uint32_t counter_count = memory_budget / ((sizeof(uint64_t) + sizeof(uint32_t) + sizeof(off_t)) * (1 / load_factor) + sizeof(uint64_t));

    MGDummy<K, T> *sketch = new MGDummy<K, T>(counter_count, f);
    return sketch;
}

template<typename K, typename T>
inline void insert_sketch(MGDummy<K, T> *sketch, K key) {
    sketch->Insert(key);
}

template<typename K, typename T>
inline void delete_sketch(MGDummy<K, T> *sketch, K key) {
    throw std::runtime_error("Deletes not implemented");
}

template<typename K, typename T>
inline int32_t query_sketch(MGDummy<K, T> *sketch, K key) {
    return sketch->Query(key);
}

template<typename K, typename T>
inline uint32_t size_of_sketch(MGDummy<K, T> *sketch) {
    return sketch->Size();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-MGDummy");

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

    const double expansion_power = parser.get<double>("--expansion-power");
    auto f = [&](size_t x) { return expansion_power == 0.0 ? std::numeric_limits<uint64_t>::max()
                                    : static_cast<uint64_t>(pow(x, 1.0 / expansion_power)); };
    auto sketch = init_sketch<uint64_t, uint32_t>(memory_budget, f);
    experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch), pass_fun(query_sketch), pass_fun(size_of_sketch));
}

