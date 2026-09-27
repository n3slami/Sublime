#pragma once

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <argparse/argparse.hpp>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "bench_utils.hpp"

#define pass_fun(f) ([](auto... args){ return f(args...); })
#define pass_ref(fun) ([](auto& f, auto... args){ return fun(f, args...); })

inline auto test_out = TestOutput();

inline std::string json_file = "";
inline uint64_t kill_exec_time_threshold = 1ULL * 3600ULL * 1000000000ULL;

inline WorkloadIO wio;
inline InputKeys<uint64_t> initial_int_keys;
inline InputKeys<std::string> initial_string_keys;
inline timer::time_point time_points[std::numeric_limits<uint8_t>::max()], decompression_time_point;
inline uint64_t timer_results[std::numeric_limits<uint8_t>::max()];
inline uint32_t top_aae_are_count = std::numeric_limits<uint32_t>::max();

/**
 * Times every single insertion, rather than a whole checkpoint's worth at
 * once, and reports the distribution's tail.
 *
 * Two clock reads per insertion cost about as much as a cuckoo-table insertion
 * itself, so a run with this on says nothing about *average* latency and must
 * not be used for it -- which is why it is opt-in and why the tail numbers come
 * from runs of their own. The tail is untouched by the overhead: what is being
 * measured there is microseconds of eviction sweep, against tens of nanoseconds
 * of instrumentation.
 */
inline bool measure_insert_latency = false;

/**
 * The insertion latencies of a run, as a histogram with 16 buckets per octave
 * (so any percentile it reports is within ~4.5%) plus an exact maximum. It is
 * never reset, so the last checkpoint of a run carries the whole run's tail.
 */
struct InsertLatencyProfile {
    static constexpr uint32_t mantissa_bits = 4;
    static constexpr uint32_t bucket_count = 64 << mantissa_bits;

    uint64_t max_ns = 0;
    uint64_t total = 0;
    std::vector<uint64_t> buckets = std::vector<uint64_t>(bucket_count, 0);

    void Add(uint64_t ns) {
        max_ns = std::max(max_ns, ns);
        total++;
        buckets[bucket_of(ns)]++;
    }

    /** @returns The smallest latency at or below which `fraction` of the insertions fell. */
    uint64_t Percentile(double fraction) const {
        if (total == 0)
            return 0;
        const uint64_t target = static_cast<uint64_t>(fraction * total);
        uint64_t seen = 0;
        for (uint32_t i = 0; i < bucket_count; i++) {
            seen += buckets[i];
            if (seen > target)
                return upper_bound_of(i);
        }
        return max_ns;
    }

private:
    static uint32_t bucket_of(uint64_t ns) {
        if (ns < (1ULL << mantissa_bits))
            return static_cast<uint32_t>(ns);
        const uint32_t octave = 63 - __builtin_clzll(ns);
        const uint32_t mantissa = (ns >> (octave - mantissa_bits)) & ((1U << mantissa_bits) - 1);
        return ((octave - mantissa_bits + 1) << mantissa_bits) | mantissa;
    }

    /** The largest latency landing in bucket `i`, which is what a percentile reports. */
    static uint64_t upper_bound_of(uint32_t i) {
        if (i < (1U << mantissa_bits))
            return i;
        const uint32_t octave = (i >> mantissa_bits) + mantissa_bits - 1;
        const uint64_t mantissa = i & ((1U << mantissa_bits) - 1);
        return (((1ULL << mantissa_bits) | mantissa) << (octave - mantissa_bits))
                    + (1ULL << (octave - mantissa_bits)) - 1;
    }
};

inline InsertLatencyProfile insert_latency;

/** Emits the insertion tail, if this run is one of the ones measuring it. */
inline void add_insert_latency_measures() {
    if (!measure_insert_latency)
        return;
    test_out.AddMeasure("max_i", insert_latency.max_ns);
    test_out.AddMeasure("p99_i", insert_latency.Percentile(0.99));
    test_out.AddMeasure("p999_i", insert_latency.Percentile(0.999));
    test_out.AddMeasure("p9999_i", insert_latency.Percentile(0.9999));
}


template <typename Sketch, typename InsertFun, typename DeleteFun, typename QueryFun, typename SizeFun> 
void experiment(Sketch *sketch, InsertFun insert_f, DeleteFun delete_f, QueryFun query_f, SizeFun size_f, void *aux_f=nullptr) {
    std::unordered_map<uint64_t, int32_t> actual_freq;
    std::vector<std::unordered_map<uint64_t, int32_t>> freq_checkpoints;
    uint32_t n_keys = 0;

    timer::time_point op_start_time = timer::now();
    while (!wio.Done()) {
        WorkloadIO::opcode opcode = wio.GetOpcode();
        switch (opcode) {
            case WorkloadIO::opcode::Insert: {
                actual_freq[wio.ReadValue<uint64_t>()]++;
                n_keys++;
                break;
            }
            case WorkloadIO::opcode::Delete: {
                actual_freq[wio.ReadValue<uint64_t>()]--;
                n_keys--;
                break;
            }
            case WorkloadIO::opcode::Flush: {
                freq_checkpoints.push_back(actual_freq);
                std::vector<uint64_t> to_remove;
                for (auto it : freq_checkpoints.back()) {
                    if (it.second == 0)
                        to_remove.push_back(it.first);
                }
                for (auto victim : to_remove)
                    freq_checkpoints.back().erase(victim);
                break;
            }
            default: {
                break;
            }
        }
    }

    wio.Reset();
    uint32_t checkpoint_ind = 0;
    n_keys = 0;
    while (!wio.Done()) {
        WorkloadIO::opcode opcode = wio.GetOpcode();
        switch (opcode) {
            case WorkloadIO::opcode::Insert: {
                const uint64_t value = wio.ReadValue<uint64_t>();
                if (measure_insert_latency) {
                    const timer::time_point start = timer::now();
                    insert_f(sketch, value);
                    insert_latency.Add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            timer::now() - start).count());
                }
                else
                    insert_f(sketch, value);
                n_keys++;
                break;
            }
            case WorkloadIO::opcode::Delete: {
                const uint64_t value = wio.ReadValue<uint64_t>();
                delete_f(sketch, value);
                n_keys--;
                break;
            }
            case WorkloadIO::opcode::Timer: {
                char timer_key = wio.ReadValue<char>();
                if (timer_results[timer_key] == 0) {
                    time_points[timer_key] = timer::now();
                    timer_results[timer_key] = -1;
                }
                else
                    timer_results[timer_key] = std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - time_points[timer_key]).count();
                break;
            }
            case WorkloadIO::opcode::Flush: {
                const uint32_t n_distinct_keys = freq_checkpoints[checkpoint_ind].size();
                double aae = 0, are = 0, con = 0;
                int64_t total_overestimation = 0, total_underestimation = 0;
                decompression_time_point = timer::now();
                time_points['q'] = timer::now();
                for (auto& it : freq_checkpoints[checkpoint_ind]) {
                    const int64_t est_val = query_f(sketch, it.first);
                    const int64_t real_val = it.second;
                    const int64_t diff = est_val - real_val;
                    const double dist = std::abs(static_cast<double>(diff));

                    aae += dist;
                    are += dist / real_val;
                    total_overestimation += std::max(diff, 0L);
                    total_underestimation -= std::min(diff, 0L);
                    con += est_val != real_val;
                }
                auto current_time = timer::now();
                timer_results['q'] = std::chrono::duration_cast<std::chrono::microseconds>(current_time - time_points['q']).count();
                timer_results['q'] += n_distinct_keys * std::max(std::chrono::duration_cast<std::chrono::microseconds>(decompression_time_point - time_points['q']).count(), 0L);

                aae /= n_distinct_keys;
                are /= n_distinct_keys;
                con /= n_distinct_keys;
                if (n_distinct_keys == 0) {
                    aae = 0;
                    are = 0;
                    con = 0;
                }

                std::vector<double> relative_errors;
                relative_errors.reserve(n_distinct_keys);
                for (auto& it : freq_checkpoints[checkpoint_ind]) {
                    const int64_t est_val = query_f(sketch, it.first);
                    const int64_t real_val = it.second;
                    const int64_t diff = est_val - real_val;
                    const double dist = std::abs(static_cast<double>(diff));
                    relative_errors.push_back(dist / real_val);
                }
                std::sort(relative_errors.begin(), relative_errors.end());

                test_out.AddMeasure("n_keys", n_keys);
                test_out.AddMeasure("n_unique_keys", n_distinct_keys);
                test_out.AddMeasure("aae", aae);
                test_out.AddMeasure("are", are);
                test_out.AddMeasure("total_overestimation", total_overestimation);
                test_out.AddMeasure("total_underestimation", total_underestimation);
                test_out.AddMeasure("p90", relative_errors[n_distinct_keys * 90 / 100]);
                test_out.AddMeasure("p95", relative_errors[n_distinct_keys * 95 / 100]);
                test_out.AddMeasure("p99", relative_errors[n_distinct_keys * 99 / 100]);
                test_out.AddMeasure("size", size_f(sketch));

                if (top_aae_are_count != std::numeric_limits<uint32_t>::max()) {
                    using FreqItem = std::pair<int64_t, uint64_t>;
                    std::priority_queue<FreqItem, std::vector<FreqItem>, std::greater<FreqItem>> top_pq;
                    for (auto& it : freq_checkpoints[checkpoint_ind]) {
                        top_pq.push({it.second, it.first});
                        if (top_pq.size() > top_aae_are_count)
                            top_pq.pop();
                    }
                    aae = 0;
                    are = 0;
                    con = 0;
                    const uint32_t total_count = top_pq.size();
                    time_points['t'] = timer::now();
                    while (!top_pq.empty()) {
                        const int64_t est_val = query_f(sketch, top_pq.top().second);
                        const int64_t real_val = top_pq.top().first;
                        const double dist = std::abs(static_cast<double>(est_val - real_val));

                        are += dist / real_val;
                        aae += dist;
                        con += est_val != real_val;
                        top_pq.pop();
                    }
                    timer_results['t'] = std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - time_points['t']).count();
                    aae /= total_count;
                    are /= total_count;
                    con /= total_count;

                    if (n_distinct_keys == 0) {
                        aae = 0;
                        are = 0;
                        con = 0;
                    }

                    test_out.AddMeasure("top_aae", aae);
                    test_out.AddMeasure("top_are", are);
                }

                if (aux_f != nullptr) {
                    std::unordered_map<std::string, uint32_t> (*converted_aux_f)(Sketch *) =
                        reinterpret_cast<std::unordered_map<std::string, uint32_t> (*)(Sketch *)>(aux_f);
                    auto aux_data = converted_aux_f(sketch);
                    for (auto [key, value] : aux_data)
                        test_out.AddMeasure(key, value);
                }
                add_insert_latency_measures();

                for (int32_t i = 0; i < std::numeric_limits<uint8_t>::max(); i++) {
                    if (timer_results[i] > 0) {
                        std::string measure_name = "time_";
                        measure_name += static_cast<char>(i);
                        test_out.AddMeasure(measure_name, timer_results[i]);
                    }
                }

                std::cout << test_out.ToJson() << ',' << std::endl;

                memset(timer_results, 0, sizeof(timer_results));
                test_out.Clear();
                checkpoint_ind++;

                if (std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - op_start_time).count() 
                            > kill_exec_time_threshold)
                    return;
                break;
            }
            case WorkloadIO::opcode::SwitchTable: {
                break;
            }
        }
    }
}


template <typename Sketch, typename InsertFun, typename DeleteFun, typename QueryFun, typename SizeFun>
void experiment_string(Sketch *sketch, InsertFun insert_f, DeleteFun delete_f, QueryFun query_f, SizeFun size_f, void *aux_f=nullptr) {
    uint16_t buf_len;
    uint8_t buf[std::numeric_limits<uint16_t>::max()];

    std::unordered_map<std::string, int32_t> actual_freq;
    std::vector<std::unordered_map<std::string, int32_t>> freq_checkpoints;
    uint32_t n_keys = 0;

    timer::time_point op_start_time = timer::now();
    while (!wio.Done()) {
        WorkloadIO::opcode opcode = wio.GetOpcode();
        switch (opcode) {
            case WorkloadIO::opcode::Insert: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                actual_freq[key]++;
                n_keys++;
                break;
            }
            case WorkloadIO::opcode::Delete: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                actual_freq[key]--;
                n_keys--;
                break;
            }
            case WorkloadIO::opcode::Flush: {
                freq_checkpoints.push_back(actual_freq);
                std::vector<std::string> to_remove;
                for (auto it : freq_checkpoints.back()) {
                    if (it.second == 0)
                        to_remove.push_back(it.first);
                }
                for (auto victim : to_remove)
                    freq_checkpoints.back().erase(victim);
                break;
            }
            default: {
                break;
            }
        }
    }

    wio.Reset();
    uint32_t checkpoint_ind = 0;
    n_keys = 0;
    while (!wio.Done()) {
        WorkloadIO::opcode opcode = wio.GetOpcode();
        switch (opcode) {
            case WorkloadIO::opcode::Insert: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                if (measure_insert_latency) {
                    const timer::time_point start = timer::now();
                    insert_f(sketch, key);
                    insert_latency.Add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            timer::now() - start).count());
                }
                else
                    insert_f(sketch, key);
                n_keys++;
                break;
            }
            case WorkloadIO::opcode::Delete: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                delete_f(sketch, key);
                n_keys--;
                break;
            }
            case WorkloadIO::opcode::Timer: {
                char timer_key = wio.ReadValue<char>();
                if (timer_results[timer_key] == 0) {
                    time_points[timer_key] = timer::now();
                    timer_results[timer_key] = -1;
                }
                else
                    timer_results[timer_key] = std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - time_points[timer_key]).count();
                break;
            }
            case WorkloadIO::opcode::Flush: {
                double aae = 0, are = 0, con = 0;
                uint64_t total_overestimation = 0, total_underestimation = 0;
                decompression_time_point = timer::now();
                time_points['q'] = timer::now();
                for (auto& it : freq_checkpoints[checkpoint_ind]) {
                    const int64_t est_val = query_f(sketch, it.first);
                    const int64_t real_val = it.second;
                    const int64_t diff = est_val - real_val;
                    const double dist = std::abs(static_cast<double>(diff));

                    aae += dist;
                    are += dist / real_val;
                    total_overestimation += std::max(diff, 0L);
                    total_underestimation -= std::min(diff, 0L);
                    con += est_val != real_val;
                }
                auto current_time = timer::now();
                timer_results['q'] = std::chrono::duration_cast<std::chrono::microseconds>(current_time - time_points['q']).count();
                const uint32_t n_distinct_keys = freq_checkpoints[checkpoint_ind].size();
                timer_results['q'] += n_distinct_keys * std::max(std::chrono::duration_cast<std::chrono::microseconds>(decompression_time_point - time_points['q']).count(), 0L);
                aae /= n_distinct_keys;
                are /= n_distinct_keys;
                con /= n_distinct_keys;

                if (n_distinct_keys == 0) {
                    aae = 0;
                    are = 0;
                    con = 0;
                }

                test_out.AddMeasure("n_keys", n_keys);
                test_out.AddMeasure("n_unique_keys", n_distinct_keys);
                test_out.AddMeasure("aae", aae);
                test_out.AddMeasure("are", are);
                test_out.AddMeasure("total_overestimation", total_overestimation);
                test_out.AddMeasure("total_underestimation", total_underestimation);

                std::vector<double> relative_errors;
                relative_errors.reserve(n_distinct_keys);
                for (auto& it : freq_checkpoints[checkpoint_ind]) {
                    const int64_t est_val = query_f(sketch, it.first);
                    const int64_t real_val = it.second;
                    const double dist = std::abs(static_cast<double>(est_val - real_val));
                    relative_errors.push_back(dist / real_val);
                }
                std::sort(relative_errors.begin(), relative_errors.end());
                if (n_distinct_keys > 0) {
                    test_out.AddMeasure("p90", relative_errors[n_distinct_keys * 90 / 100]);
                    test_out.AddMeasure("p95", relative_errors[n_distinct_keys * 95 / 100]);
                    test_out.AddMeasure("p99", relative_errors[n_distinct_keys * 99 / 100]);
                }
                test_out.AddMeasure("size", size_f(sketch));

                if (top_aae_are_count != std::numeric_limits<uint32_t>::max()) {
                    using FreqItem = std::pair<int64_t, std::string>;
                    std::priority_queue<FreqItem, std::vector<FreqItem>, std::greater<FreqItem>> top_pq;
                    for (auto& it : freq_checkpoints[checkpoint_ind]) {
                        top_pq.push({it.second, it.first});
                        if (top_pq.size() > top_aae_are_count)
                            top_pq.pop();
                    }
                    aae = 0;
                    are = 0;
                    con = 0;
                    const uint32_t total_count = top_pq.size();
                    time_points['t'] = timer::now();
                    while (!top_pq.empty()) {
                        const int64_t est_val = query_f(sketch, top_pq.top().second);
                        const int64_t real_val = top_pq.top().first;
                        const double dist = std::abs(static_cast<double>(est_val - real_val));

                        are += dist / real_val;
                        aae += dist;
                        con += est_val != real_val;
                        top_pq.pop();
                    }
                    timer_results['t'] = std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - time_points['t']).count();
                    aae /= total_count;
                    are /= total_count;
                    con /= total_count;

                    if (n_distinct_keys == 0) {
                        aae = 0;
                        are = 0;
                        con = 0;
                    }

                    test_out.AddMeasure("top_aae", aae);
                    test_out.AddMeasure("top_are", are);
                }

                if (aux_f != nullptr) {
                    std::unordered_map<std::string, uint32_t> (*converted_aux_f)(Sketch *) =
                        reinterpret_cast<std::unordered_map<std::string, uint32_t> (*)(Sketch *)>(aux_f);
                    auto aux_data = converted_aux_f(sketch);
                    for (auto [key, value] : aux_data)
                        test_out.AddMeasure(key, value);
                }
                add_insert_latency_measures();

                for (int32_t i = 0; i < std::numeric_limits<uint8_t>::max(); i++) {
                    if (timer_results[i] > 0) {
                        std::string measure_name = "time_";
                        measure_name += static_cast<char>(i);
                        test_out.AddMeasure(measure_name, timer_results[i]);
                    }
                }

                std::cout << test_out.ToJson() << ',' << std::endl;

                memset(timer_results, 0, sizeof(timer_results));
                test_out.Clear();
                checkpoint_ind++;

                if (std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - op_start_time).count() 
                            > kill_exec_time_threshold)
                    return;
                break;
            }
            case WorkloadIO::opcode::SwitchTable: {
                break;
            }
        }
    }
}


template <typename Sketch, typename InsertFun, typename DeleteFun, typename QueryFun, typename SizeFun>
void experiment_join_string(std::vector<Sketch *> sketches, InsertFun insert_f, DeleteFun delete_f, QueryFun query_f, SizeFun size_f) {
    uint16_t buf_len;
    uint8_t buf[std::numeric_limits<uint16_t>::max()];

    std::vector<std::unordered_map<std::string, int32_t>> actual_freq;
    actual_freq.resize(sketches.size());
    std::vector<std::vector<std::unordered_map<std::string, int32_t>>> freq_checkpoints;
    uint32_t n_keys = 0, table_ind = 0;

    timer::time_point op_start_time = timer::now();
    while (!wio.Done()) {
        WorkloadIO::opcode opcode = wio.GetOpcode();
        switch (opcode) {
            case WorkloadIO::opcode::Insert: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                actual_freq[table_ind][key]++;
                n_keys++;
                break;
            }
            case WorkloadIO::opcode::Delete: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                actual_freq[table_ind][key]--;
                n_keys--;
                break;
            }
            case WorkloadIO::opcode::Flush: {
                freq_checkpoints.push_back(actual_freq);
                std::vector<std::pair<int32_t, std::string>> to_remove;
                for (int32_t remove_table_ind = 0; remove_table_ind < sketches.size(); remove_table_ind++) {
                    for (auto it : freq_checkpoints.back()[remove_table_ind]) {
                        if (it.second == 0)
                            to_remove.emplace_back(remove_table_ind, it.first);
                    }
                }
                for (auto [victim_table, victim] : to_remove)
                    freq_checkpoints.back()[victim_table].erase(victim);
                break;
            }
            case WorkloadIO::opcode::SwitchTable: {
                table_ind = wio.ReadValue<int32_t>();
                if (table_ind > sketches.size())
                    throw std::runtime_error("Table index exceeds number of tables/sketches provided in experiment.");
                break;
            }
            default: {
                break;
            }
        }
    }

    wio.Reset();
    uint32_t checkpoint_ind = 0;
    n_keys = 0;
    table_ind = 0;
    while (!wio.Done()) {
        WorkloadIO::opcode opcode = wio.GetOpcode();
        switch (opcode) {
            case WorkloadIO::opcode::Insert: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                insert_f(sketches[table_ind], key);
                n_keys++;
                break;
            }
            case WorkloadIO::opcode::Delete: {
                wio.GetStringKey(buf_len, buf);
                const std::string key(buf, buf + buf_len);
                delete_f(sketches[table_ind], key);
                n_keys--;
                break;
            }
            case WorkloadIO::opcode::Timer: {
                char timer_key = wio.ReadValue<char>();
                if (timer_results[timer_key] == 0) {
                    time_points[timer_key] = timer::now();
                    timer_results[timer_key] = -1;
                }
                else
                    timer_results[timer_key] = std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - time_points[timer_key]).count();
                break;
            }
            case WorkloadIO::opcode::Flush: {
                std::map<std::string, int64_t> join_result;       // Only evaluate the actual joins
                for (auto& it : freq_checkpoints[checkpoint_ind][0])
                    join_result[it.first] = it.second;
                for (int32_t table = 1; table < sketches.size(); table++) {
                    for (auto& it : join_result)
                        join_result[it.first] *= freq_checkpoints[checkpoint_ind][table][it.first];
                }
                std::vector<std::string> join_results_to_remove;
                for (auto it : join_result) {
                    if (it.second == 0)
                        join_results_to_remove.push_back(it.first);
                }
                for (auto& it : join_results_to_remove)
                    join_result.erase(it);

                // Output stat arrays
                std::vector<double> aae_vec, are_vec, con_vec;
                std::vector<uint64_t> total_overestimation_vec, total_underestimation_vec, query_times_vec;
                std::vector<uint32_t> n_distinct_keys_vec;

                // Get the stats for each table
                for (int32_t i = 0; i < sketches.size(); i++) {
                    double aae = 0, are = 0, con = 0;
                    uint64_t total_overestimation = 0, total_underestimation = 0;
                    time_points['q'] = timer::now();
                    for (auto& it : freq_checkpoints[checkpoint_ind][i]) {
                        const int64_t est_val = query_f(sketches[i], it.first);
                        const int64_t real_val = it.second;
                        if (real_val == 0)
                            continue;
                        const int64_t diff = est_val - real_val;
                        const double dist = std::abs(static_cast<double>(diff));

                        aae += dist;
                        are += dist / real_val;
                        total_overestimation += std::max(diff, 0L);
                        total_underestimation -= std::min(diff, 0L);
                        con += est_val != real_val;
                    }
                    auto current_time = timer::now();
                    timer_results['q'] = std::chrono::duration_cast<std::chrono::microseconds>(current_time - time_points['q']).count();
                    const uint32_t n_distinct_keys = freq_checkpoints[checkpoint_ind][i].size();
                    timer_results['q'] += n_distinct_keys * std::max(std::chrono::duration_cast<std::chrono::microseconds>(decompression_time_point - time_points['q']).count(), 0L);
                    aae /= n_distinct_keys;
                    are /= n_distinct_keys;
                    con /= n_distinct_keys;

                    if (n_distinct_keys == 0) {
                        aae = 0;
                        are = 0;
                        con = 0;
                    }

                    aae_vec.push_back(aae);
                    are_vec.push_back(are);
                    con_vec.push_back(con);
                    total_overestimation_vec.push_back(total_overestimation);
                    total_underestimation_vec.push_back(total_underestimation);
                    query_times_vec.push_back(timer_results['q']);
                    n_distinct_keys_vec.push_back(n_distinct_keys);
                }

                // Get the stats for the join result
                double aae = 0, are = 0, con = 0;
                uint64_t total_overestimation = 0, total_underestimation = 0;
                time_points['q'] = timer::now();
                for (auto& it : join_result) {
                    int64_t est_val = 1;
                    for (int32_t i = 0; i < sketches.size(); i++)
                        est_val *= query_f(sketches[i], it.first);
                    const int64_t real_val = it.second;
                    const int64_t diff = est_val - real_val;
                    const double dist = std::abs(static_cast<double>(diff));

                    aae += dist;
                    are += dist / real_val;
                    total_overestimation += std::max(diff, 0L);
                    total_underestimation -= std::min(diff, 0L);
                    con += est_val != real_val;
                }
                auto current_time = timer::now();
                timer_results['q'] = std::chrono::duration_cast<std::chrono::microseconds>(current_time - time_points['q']).count();
                const uint32_t n_distinct_keys = join_result.size();
                timer_results['q'] += n_distinct_keys * std::max(std::chrono::duration_cast<std::chrono::microseconds>(decompression_time_point - time_points['q']).count(), 0L);
                aae /= n_distinct_keys;
                are /= n_distinct_keys;
                con /= n_distinct_keys;

                if (n_distinct_keys == 0) {
                    aae = 0;
                    are = 0;
                    con = 0;
                }

                aae_vec.push_back(aae);
                are_vec.push_back(are);
                con_vec.push_back(con);
                total_overestimation_vec.push_back(total_overestimation);
                total_underestimation_vec.push_back(total_underestimation);
                query_times_vec.push_back(timer_results['q']);
                n_distinct_keys_vec.push_back(n_distinct_keys);

                test_out.AddMeasure("n_keys", n_keys);
                test_out.AddMeasure("n_unique_keys", n_distinct_keys_vec);
                test_out.AddMeasure("aae", aae_vec);
                test_out.AddMeasure("are", are_vec);
                test_out.AddMeasure("total_overestimation", total_overestimation_vec);
                test_out.AddMeasure("total_underestimation", total_underestimation_vec);
                std::vector<uint32_t> sketch_sizes;
                for (int32_t i = 0; i < sketches.size(); i++)
                    sketch_sizes.push_back(size_f(sketches[i]));
                test_out.AddMeasure("size", sketch_sizes);

                for (int32_t i = 0; i < std::numeric_limits<uint8_t>::max(); i++) {
                    std::string measure_name = "time_";
                    measure_name += static_cast<char>(i);
                    if (i == static_cast<uint8_t>('q'))
                        test_out.AddMeasure(measure_name, query_times_vec);
                    else if (timer_results[i] > 0)
                        test_out.AddMeasure(measure_name, timer_results[i]);
                }

                std::cout << test_out.ToJson() << ',' << std::endl;

                memset(timer_results, 0, sizeof(timer_results));
                test_out.Clear();
                checkpoint_ind++;

                if (std::chrono::duration_cast<std::chrono::microseconds>(timer::now() - op_start_time).count() 
                            > kill_exec_time_threshold)
                    return;
                break;
            }
            case WorkloadIO::opcode::SwitchTable: {
                table_ind = wio.ReadValue<int32_t>();
                break;
            }
        }
    }
}


inline argparse::ArgumentParser init_parser(const std::string& name) {
    argparse::ArgumentParser parser(name);

    parser.add_argument("arg")
            .help("the initial memory budget of the sketch, in bytes")
            .required()
            .nargs(argparse::nargs_pattern::at_least_one)
            .scan<'u', uint64_t>();

    parser.add_argument("--size-function-power")
            .help("the exponent of N the size function")
            .nargs(1)
            .default_value(static_cast<double>(0.0))
            .scan<'g', double>();

    parser.add_argument("--size-function-mult")
            .help("the divisor (epsilon) in the size function")
            .nargs(1)
            .default_value(static_cast<double>(0.0))
            .scan<'g', double>();

    parser.add_argument("--l2-size-function")
          .help("use the l2-norm for the size function")
          .default_value(false)
          .implicit_value(true);

    parser.add_argument("-r", "--rows")
            .help("the number of rows in the sketch")
            .nargs(1)
            .default_value(static_cast<uint32_t>(3))
            .scan<'u', uint32_t>();

    parser.add_argument("--counter-count")
            .help("the number of counters in the sketch, fixes it in advance")
            .nargs(1)
            .default_value(static_cast<uint32_t>(0))
            .scan<'u', uint32_t>();

    parser.add_argument("-w", "--workload")
            .help("pass the workload from file")
            .nargs(1);

    parser.add_argument("-k", "--keys")
            .help("pass the keys from file")
            .nargs(1);

    return parser;
}


inline void read_workload(const std::string& workload_file) {
    wio = WorkloadIO(workload_file, WorkloadIO::iomode::Read);
}


inline void print_test() {
    std::cout << test_out.ToJson() << std::endl;
}

