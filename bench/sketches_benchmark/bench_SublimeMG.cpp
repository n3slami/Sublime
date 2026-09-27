#include <chrono>
#include <type_traits>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

#include "../bench_template.hpp"
#include "CuckooTable.hpp"
#include "SublimeMG.hpp"

using sublime::SublimeMG;

static uint32_t g_fingerprint_length = 32;
static uint32_t g_growth_coefficient = 1;
static std::function<uint64_t(double)> g_size_function;
static uint32_t g_seed = 0;
static bool g_vale_retuning = true;

/** The `key_bits` that make a freshly stored fingerprint `fp_len` bits long. */
static uint64_t key_bits_for(uint64_t nslots, uint32_t fp_len) {
    return sublime::CuckooTable::KeyBitsFor(nslots, fp_len, g_growth_coefficient);
}

/** Hashes a string key to the 64-bit value the table then treats as its hash. */
static inline uint64_t hash_string(const std::string& key) {
    return sublime::MurmurHash64A(key.data(), static_cast<int32_t>(key.size()), 0x5bd1e995u);
}

/** The largest `nslots` whose empty sketch fits inside `budget` bytes. */
template <bool E, bool T>
static uint64_t nslots_for_budget(uint64_t budget) {
    const auto size_at = [&](uint64_t nslots) {
        SublimeMG<E, T> probe(nslots, key_bits_for(nslots, g_fingerprint_length),
                           SublimeMG<E, T>::hashmode::Default, 1, g_growth_coefficient);
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

template <bool E, bool T>
inline void insert_sketch(SublimeMG<E, T> *sketch, const std::string& key) {
    sketch->Insert(hash_string(key), SublimeMG<E, T>::flag_key_is_hash);
}
template <bool E, bool T, typename K>
inline void insert_sketch(SublimeMG<E, T> *sketch, K key) {
    sketch->Insert(static_cast<uint64_t>(key));
}

template <bool E, bool T>
inline void delete_sketch(SublimeMG<E, T> *, const std::string&) {
    throw std::runtime_error("Deletes not implemented");
}
template <bool E, bool T, typename K>
inline void delete_sketch(SublimeMG<E, T> *, K) {
    throw std::runtime_error("Deletes not implemented");
}

template <bool E, bool T>
inline int32_t query_sketch(SublimeMG<E, T> *sketch, const std::string& key) {
    return sketch->Query(hash_string(key), SublimeMG<E, T>::flag_key_is_hash);
}
template <bool E, bool T, typename K>
inline int32_t query_sketch(SublimeMG<E, T> *sketch, K key) {
    return sketch->Query(static_cast<uint64_t>(key));
}

template <bool E, bool T>
inline uint32_t size_of_sketch(SublimeMG<E, T> *sketch) {
    return sketch->SizeInBytes();
}

template <bool E, bool T>
inline std::unordered_map<std::string, uint32_t> get_extra_parameters(SublimeMG<E, T> *sketch) {
    std::unordered_map<std::string, uint32_t> res;
    res["expansions"] = sketch->CountExpansions();
    res["monitored"] = sketch->CountMonitored();
    res["capacity"] = sketch->Capacity();
    res["error_inducing"] = sketch->CountErrorInducingInsertions();
    res["counters_per_chunk"] = sketch->Counters().GetCountersPerChunk();
    res["stub_length"] = sketch->Counters().GetStubLength();
    res["min_tree"] = T ? 1 : 0;
    res["vale_retuning"] = sketch->GetVALERetuning() ? 1 : 0;
    res["fingerprint_bits"] = sketch->GetTable().GetNumFingerprintBits();
    // A cuckoo filter drops an entry when a kick path gives up on it, which is
    // silent accuracy loss -- so it is reported rather than left to be guessed.
    res["lost_entries"] = sketch->GetTable().CountLostEntries();
    return res;
}

template <bool E, bool T>
static void run(uint64_t budget) {
    const uint64_t nslots = nslots_for_budget<E, T>(budget);
    auto *sketch = new SublimeMG<E, T>(nslots, key_bits_for(nslots, g_fingerprint_length),
                                    SublimeMG<E, T>::hashmode::Default, g_seed,
                                    g_growth_coefficient, g_size_function);
    sketch->SetVALERetuning(g_vale_retuning);
    top_aae_are_count = sketch->Capacity();
    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                           pass_fun(query_sketch), pass_fun(size_of_sketch),
                           reinterpret_cast<void *>(get_extra_parameters<E, T>));
    else
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                   pass_fun(query_sketch), pass_fun(size_of_sketch),
                   reinterpret_cast<void *>(get_extra_parameters<E, T>));
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-SublimeMG");
    parser.add_argument("--fingerprint-length")
            .help("the length, in bits, of a stored fingerprint")
            .nargs(1).default_value(static_cast<uint32_t>(32)).scan<'u', uint32_t>();
    parser.add_argument("--growth-coefficient")
            .help("r: expansions before a full doubling")
            .nargs(1).default_value(static_cast<uint32_t>(1)).scan<'u', uint32_t>();
    parser.add_argument("--expand-measure")
            .help("what the size function is tested against: 'error' (error-inducing insertions) or 'total'")
            .nargs(1).default_value(std::string("error"));
    parser.add_argument("--min-tree")
            .help("lay a min segment tree over the counters: O(log w) evictions, twice the counters")
            .default_value(false).implicit_value(true);
    parser.add_argument("--tail-latency")
            .help("time every insertion and report the tail; the average latency of such a run "
                  "is meaningless, so this wants a run of its own")
            .default_value(false).implicit_value(true);
    parser.add_argument("--no-retune")
            .help("leave VALE on the tuning it was constructed with, instead of re-deriving it "
                  "as the counts move")
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
    g_growth_coefficient = parser.get<uint32_t>("--growth-coefficient");
    measure_insert_latency = parser.get<bool>("--tail-latency");
    g_vale_retuning = !parser.get<bool>("--no-retune");
    g_seed = parser.get<uint32_t>("--seed");
    if (g_seed == 0)
        g_seed = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::now())
                    .time_since_epoch().count();

    const double power = parser.get<double>("--size-function-power");
    const double mult = parser.get<double>("--size-function-mult");
    g_size_function = [power, mult](double x) {
        return power == 0.0 ? std::numeric_limits<uint64_t>::max()
                            : static_cast<uint64_t>(pow(x, 1.0 / power) * mult);
    };

    if (memory_budgets.size() != 1)
        throw std::runtime_error("SublimeMG benchmark takes a single memory budget");

    const bool error_measure = parser.get<std::string>("--expand-measure") != "total";
    const bool tree = parser.get<bool>("--min-tree");
    if (error_measure && tree)        run<true, true>(memory_budgets[0]);
    else if (error_measure)           run<true, false>(memory_budgets[0]);
    else if (tree)                    run<false, true>(memory_budgets[0]);
    else                              run<false, false>(memory_budgets[0]);
}
