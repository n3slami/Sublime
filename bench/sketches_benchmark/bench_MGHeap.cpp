#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../bench_template.hpp"
#include "MGHeap.hpp"

using sublime::MGHeap;

static uint32_t g_fingerprint_length = MGHeap::default_fingerprint_length;
static uint32_t g_seed = 0;

static inline uint64_t hash_string(const std::string& key) {
    return sublime::MurmurHash64A(key.data(), static_cast<int32_t>(key.size()), 0x5bd1e995u);
}

/** The largest `nslots` whose empty summary fits inside `budget` bytes. */
static uint64_t nslots_for_budget(uint64_t budget) {
    const auto size_at = [&](uint64_t nslots) {
        MGHeap probe(nslots, MGHeap::hashmode::Default, 1, g_fingerprint_length);
        return probe.SizeInBytes();
    };
    uint64_t lo = 16, hi = 16;
    while (size_at(hi) < budget && hi < (1ULL << 34))
        hi <<= 1;
    uint64_t best = lo;
    while (lo <= hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        if (size_at(mid) <= budget) {
            best = mid;
            lo = mid + 1;
        }
        else {
            if (mid == 0)
                break;
            hi = mid - 1;
        }
    }
    return best;
}

inline void insert_sketch(MGHeap *sketch, const std::string& key) {
    sketch->Insert(hash_string(key), MGHeap::flag_key_is_hash);
}
template <typename T>
inline void insert_sketch(MGHeap *sketch, T key) {
    sketch->Insert(static_cast<uint64_t>(key));
}

inline void delete_sketch(MGHeap *, const std::string&) {
    throw std::runtime_error("Deletes not implemented");
}
template <typename T>
inline void delete_sketch(MGHeap *, T) {
    throw std::runtime_error("Deletes not implemented");
}

inline int32_t query_sketch(MGHeap *sketch, const std::string& key) {
    return sketch->Query(hash_string(key), MGHeap::flag_key_is_hash);
}
template <typename T>
inline int32_t query_sketch(MGHeap *sketch, T key) {
    return sketch->Query(static_cast<uint64_t>(key));
}

inline uint32_t size_of_sketch(MGHeap *sketch) {
    return sketch->SizeInBytes();
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-MGHeap");
    parser.add_argument("--fingerprint-length")
            .help("the length, in bits, of a stored fingerprint")
            .nargs(1).default_value(static_cast<uint32_t>(MGHeap::default_fingerprint_length))
            .scan<'u', uint32_t>();
    parser.add_argument("--seed")
            .help("hash seed; 0 (the default) uses a time-based seed")
            .nargs(1).default_value(static_cast<uint32_t>(0)).scan<'u', uint32_t>();

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
    g_fingerprint_length = parser.get<uint32_t>("--fingerprint-length");
    g_seed = parser.get<uint32_t>("--seed");
    if (g_seed == 0)
        g_seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())
                    .time_since_epoch().count();

    const uint64_t nslots = nslots_for_budget(memory_budgets[0]);
    auto *sketch = new MGHeap(nslots, MGHeap::hashmode::Default, g_seed, g_fingerprint_length);
    top_aae_are_count = sketch->Capacity();

    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                           pass_fun(query_sketch), pass_fun(size_of_sketch));
    else
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                   pass_fun(query_sketch), pass_fun(size_of_sketch));
}
