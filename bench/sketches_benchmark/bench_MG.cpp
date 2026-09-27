#include <chrono>
#include <type_traits>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "../bench_template.hpp"
#include "CuckooTable.hpp"
#include "MG.hpp"

using sublime::MG;

static uint32_t g_fingerprint_length = MG<>::default_fingerprint_length;
static uint32_t g_seed = 0;

static inline uint64_t hash_string(const std::string& key) {
    return sublime::MurmurHash64A(key.data(), static_cast<int32_t>(key.size()), 0x5bd1e995u);
}

/** The largest `nslots` whose empty summary fits inside `budget` bytes. */
template <typename Tab>
static uint64_t nslots_for_budget(uint64_t budget) {
    const auto size_at = [&](uint64_t nslots) {
        MG<Tab> probe(nslots, MG<Tab>::hashmode::Default, 1, g_fingerprint_length);
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

template <typename Tab>
inline void insert_sketch(MG<Tab> *sketch, const std::string& key) {
    sketch->Insert(hash_string(key), MG<Tab>::flag_key_is_hash);
}
template <typename Tab, typename T>
inline void insert_sketch(MG<Tab> *sketch, T key) {
    sketch->Insert(static_cast<uint64_t>(key));
}

template <typename Tab>
inline void delete_sketch(MG<Tab> *, const std::string&) {
    throw std::runtime_error("Deletes not implemented");
}
template <typename Tab, typename T>
inline void delete_sketch(MG<Tab> *, T) {
    throw std::runtime_error("Deletes not implemented");
}

template <typename Tab>
inline int32_t query_sketch(MG<Tab> *sketch, const std::string& key) {
    return sketch->Query(hash_string(key), MG<Tab>::flag_key_is_hash);
}
template <typename Tab, typename T>
inline int32_t query_sketch(MG<Tab> *sketch, T key) {
    return sketch->Query(static_cast<uint64_t>(key));
}

template <typename Tab>
inline uint32_t size_of_sketch(MG<Tab> *sketch) {
    return sketch->SizeInBytes();
}


template <typename Tab>
inline std::unordered_map<std::string, uint32_t> get_extra_parameters(MG<Tab> *sketch) {
    std::unordered_map<std::string, uint32_t> res;
    res["monitored"] = sketch->CountMonitored();
    res["capacity"] = sketch->Capacity();
    res["decrements"] = sketch->CountDecrements();
    res["cuckoo"] = std::is_same<Tab, sublime::CuckooTable>::value ? 1 : 0;
    // See `bench_SublimeMG`: a cuckoo filter can drop an entry on a kick path.
    if constexpr (std::is_same<Tab, sublime::CuckooTable>::value)
        res["lost_entries"] = sketch->GetTable().CountLostEntries();
    return res;
}

template <typename Tab>
static void run(uint64_t budget) {
    const uint64_t nslots = nslots_for_budget<Tab>(budget);
    auto *sketch = new MG<Tab>(nslots, MG<Tab>::hashmode::Default, g_seed, g_fingerprint_length);
    top_aae_are_count = sketch->Capacity();

    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                           pass_fun(query_sketch), pass_fun(size_of_sketch),
                           reinterpret_cast<void *>(get_extra_parameters<Tab>));
    else
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                   pass_fun(query_sketch), pass_fun(size_of_sketch),
                   reinterpret_cast<void *>(get_extra_parameters<Tab>));
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-MG");
    parser.add_argument("--cuckoo")
            .help("keep the monitored keys in a cuckoo filter rather than a quotient filter")
            .default_value(false).implicit_value(true);
    parser.add_argument("--fingerprint-length")
            .help("the length, in bits, of a stored fingerprint")
            .nargs(1).default_value(static_cast<uint32_t>(MG<>::default_fingerprint_length))
            .scan<'u', uint32_t>();
    parser.add_argument("--tail-latency")
            .help("time every insertion and report the tail; the average latency of such a run "
                  "is meaningless, so this wants a run of its own")
            .default_value(false).implicit_value(true);
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
    measure_insert_latency = parser.get<bool>("--tail-latency");
    g_seed = parser.get<uint32_t>("--seed");
    if (g_seed == 0)
        g_seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())
                    .time_since_epoch().count();

    if (parser.get<bool>("--cuckoo"))
        run<sublime::CuckooTable>(memory_budgets[0]);
    else
        run<sublime::FingerprintTable>(memory_budgets[0]);
}
