#include <cstdint>
#include <stdexcept>
#include <string>

#include "../bench_template.hpp"
#include "SpaceSaving.hpp"
#include "TableHashing.hpp"   // for sublime::MurmurHash64A

using sublime::SpaceSaving;

static inline uint64_t hash_string(const std::string& key) {
    return sublime::MurmurHash64A(key.data(), static_cast<int32_t>(key.size()), 0x5bd1e995u);
}

inline void insert_sketch(SpaceSaving *sketch, const std::string& key) {
    sketch->Insert(hash_string(key));
}
template <typename T>
inline void insert_sketch(SpaceSaving *sketch, T key) {
    sketch->Insert(static_cast<uint64_t>(key));
}

inline void delete_sketch(SpaceSaving *, const std::string&) {
    throw std::runtime_error("Deletes not implemented");
}
template <typename T>
inline void delete_sketch(SpaceSaving *, T) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(SpaceSaving *sketch, const std::string& key) {
    return sketch->Query(hash_string(key));
}
template <typename T>
inline int32_t query_sketch(SpaceSaving *sketch, T key) {
    return sketch->Query(static_cast<uint64_t>(key));
}

inline uint32_t size_of_sketch(SpaceSaving *sketch) {
    return sketch->SizeInBytes();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-SpaceSaving");

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

    const uint64_t capacity = std::max<uint64_t>(1, memory_budgets[0] / SpaceSaving::BytesPerMonitorEstimate());
    auto *sketch = new SpaceSaving(capacity);
    top_aae_are_count = sketch->Capacity();

    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                           pass_fun(query_sketch), pass_fun(size_of_sketch));
    else
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                   pass_fun(query_sketch), pass_fun(size_of_sketch));
}
