#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>

#include "../bench_template.hpp"
#include "SublimeMG.hpp"

using sublime::SublimeMG;

static uint32_t g_fingerprint_length = 10;
static uint32_t g_growth_coefficient = 1;
static std::function<uint64_t(double)> g_size_function;
static uint32_t g_seed = 0;

/** The `key_bits` that make a freshly stored fingerprint `fp_len` bits long. */
static uint64_t key_bits_for(uint64_t nslots, uint32_t fp_len) {
    uint64_t quotient_bits = 0;
    for (uint64_t n = nslots; n > 1; n >>= 1)
        quotient_bits++;
    quotient_bits += (__builtin_popcountll(nslots) > 1);
    return quotient_bits + fp_len;
}

/** Hashes a string key to the 64-bit value the table then treats as its hash. */
static inline uint64_t hash_string(const std::string& key) {
    return sublime::MurmurHash64A(key.data(), static_cast<int32_t>(key.size()), 0x5bd1e995u);
}

/** The largest `nslots` whose empty sketch fits inside `budget` bytes. */
template <bool E>
static uint64_t nslots_for_budget(uint64_t budget) {
    const auto size_at = [&](uint64_t nslots) {
        SublimeMG<E> probe(nslots, key_bits_for(nslots, g_fingerprint_length),
                           SublimeMG<E>::hashmode::Default, 1, g_growth_coefficient);
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

template <bool E>
inline void insert_sketch(SublimeMG<E> *sketch, const std::string& key) {
    sketch->Insert(hash_string(key), SublimeMG<E>::flag_key_is_hash);
}
template <bool E, typename T>
inline void insert_sketch(SublimeMG<E> *sketch, T key) {
    sketch->Insert(static_cast<uint64_t>(key));
}

template <bool E>
inline void delete_sketch(SublimeMG<E> *, const std::string&) {
    throw std::runtime_error("Deletes not implemented");
}
template <bool E, typename T>
inline void delete_sketch(SublimeMG<E> *, T) {
    throw std::runtime_error("Deletes not implemented");
}

// A query has to see the buffered insertions, but only the first query of a
// checkpoint pays for the flush -- the rest find the buffer already empty.
template <bool E>
inline int32_t query_sketch(SublimeMG<E> *sketch, const std::string& key) {
    if (sketch->CountBuffered() > 0)
        sketch->FlushBuffer();
    return sketch->Query(hash_string(key), SublimeMG<E>::flag_key_is_hash);
}
template <bool E, typename T>
inline int32_t query_sketch(SublimeMG<E> *sketch, T key) {
    if (sketch->CountBuffered() > 0)
        sketch->FlushBuffer();
    return sketch->Query(static_cast<uint64_t>(key));
}

template <bool E>
inline uint32_t size_of_sketch(SublimeMG<E> *sketch) {
    return sketch->SizeInBytes();
}

template <bool E>
inline std::unordered_map<std::string, uint32_t> get_extra_parameters(SublimeMG<E> *sketch) {
    std::unordered_map<std::string, uint32_t> res;
    res["expansions"] = sketch->CountExpansions();
    res["monitored"] = sketch->CountMonitored();
    res["capacity"] = sketch->Capacity();
    res["error_inducing"] = sketch->CountErrorInducingInsertions();
    return res;
}

template <bool E>
static void run(uint64_t budget) {
    const uint64_t nslots = nslots_for_budget<E>(budget);
    auto *sketch = new SublimeMG<E>(nslots, key_bits_for(nslots, g_fingerprint_length),
                                    SublimeMG<E>::hashmode::Default, g_seed,
                                    g_growth_coefficient,
                                    SublimeMG<E>::default_buffer_capacity,
                                    g_size_function);
    top_aae_are_count = sketch->Capacity();
    if (wio.StringKeys())
        experiment_string(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                           pass_fun(query_sketch), pass_fun(size_of_sketch),
                           reinterpret_cast<void *>(get_extra_parameters<E>));
    else
        experiment(sketch, pass_fun(insert_sketch), pass_fun(delete_sketch),
                   pass_fun(query_sketch), pass_fun(size_of_sketch),
                   reinterpret_cast<void *>(get_extra_parameters<E>));
}


int main(int argc, char const *argv[]) {
    auto parser = init_parser("bench-SublimeMG");
    parser.add_argument("--fingerprint-length")
            .help("the length, in bits, of a stored fingerprint")
            .nargs(1).default_value(static_cast<uint32_t>(10)).scan<'u', uint32_t>();
    parser.add_argument("--growth-coefficient")
            .help("r: expansions before a full doubling")
            .nargs(1).default_value(static_cast<uint32_t>(1)).scan<'u', uint32_t>();
    parser.add_argument("--expand-measure")
            .help("what the size function is tested against: 'error' (error-inducing insertions) or 'total'")
            .nargs(1).default_value(std::string("error"));

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

    if (parser.get<std::string>("--expand-measure") == "total")
        run<false>(memory_budgets[0]);
    else
        run<true>(memory_budgets[0]);
}
