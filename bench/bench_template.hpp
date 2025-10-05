#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <argparse/argparse.hpp>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include "bench_utils.hpp"

#define pass_fun(f) ([](auto... args){ return f(args...); })
#define pass_ref(fun) ([](auto& f, auto... args){ return fun(f, args...); })

inline auto test_out = TestOutput();

inline std::string json_file = "";
inline uint64_t memory_budget;
inline uint64_t kill_exec_time_threshold = 1ULL * 3600ULL * 1000000ULL;

inline WorkloadIO wio;
inline InputKeys<uint64_t> initial_int_keys;
inline InputKeys<std::string> initial_string_keys;
inline timer::time_point time_points[std::numeric_limits<uint8_t>::max()];
inline uint64_t timer_results[std::numeric_limits<uint8_t>::max()];
inline uint32_t top_aae_are_count = std::numeric_limits<uint32_t>::max();


template <typename Sketch, typename InsertFun, typename DeleteFun, typename QueryFun, typename SizeFun>
void experiment(Sketch *sketch, InsertFun insert_f, DeleteFun delete_f, QueryFun query_f, SizeFun size_f) {
    std::unordered_map<uint64_t, uint32_t> actual_freq;
    std::vector<std::unordered_map<uint64_t, uint32_t>> freq_checkpoints;
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
                insert_f(sketch, wio.ReadValue<uint64_t>());
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
                    timer_results[timer_key] = std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - time_points[timer_key]).count();
                break;
            }
            case WorkloadIO::opcode::Flush: {
                double aae = 0, are = 0, con = 0;
                int64_t total_overestimation = 0, total_underestimation = 0;
                time_points['q'] = timer::now();
                for (auto& it : freq_checkpoints[checkpoint_ind]) {
                    const int64_t est_val = query_f(sketch, it.first);
                    const int64_t real_val = it.second;
                    const int64_t diff = est_val - real_val;
                    const double dist = std::abs(static_cast<double>(diff));

                    aae += dist;
                    are += dist / real_val;
                    total_overestimation += std::max(diff, 0L);
                    total_underestimation -= std::max(diff, 0L);
                    con += est_val != real_val;
                }
                timer_results['q'] = std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - time_points['q']).count();
                const uint32_t n_distinct_keys = freq_checkpoints[checkpoint_ind].size();
                aae /= n_distinct_keys;
                are /= n_distinct_keys;
                con /= n_distinct_keys;

                test_out.AddMeasure("n_keys", n_keys);
                test_out.AddMeasure("aae", aae);
                test_out.AddMeasure("are", are);
                test_out.AddMeasure("total_overestimation", total_overestimation);
                test_out.AddMeasure("total_underestimation", total_underestimation);
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
                    timer_results['t'] = std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - time_points['t']).count();
                    aae /= total_count;
                    are /= total_count;
                    con /= total_count;

                    test_out.AddMeasure("top_aae", aae);
                    test_out.AddMeasure("top_are", are);
                }

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

                if (std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - op_start_time).count() 
                            > kill_exec_time_threshold)
                    return;
                break;
            }
        }
    }
}


template <typename Sketch, typename InsertFun, typename DeleteFun, typename QueryFun, typename SizeFun>
void experiment_string(Sketch *sketch, InsertFun insert_f, DeleteFun delete_f, QueryFun query_f, SizeFun size_f) {
    uint16_t buf_len;
    uint8_t buf[std::numeric_limits<uint16_t>::max()];

    std::unordered_map<std::string, uint32_t> actual_freq;
    std::vector<std::unordered_map<std::string, uint32_t>> freq_checkpoints;
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
                    timer_results[timer_key] = std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - time_points[timer_key]).count();
                break;
            }
            case WorkloadIO::opcode::Flush: {
                double aae = 0, are = 0, con = 0;
                uint64_t total_overestimation = 0, total_underestimation = 0;
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
                timer_results['q'] = std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - time_points['q']).count();
                aae /= freq_checkpoints[checkpoint_ind].size();
                are /= freq_checkpoints[checkpoint_ind].size();
                con /= freq_checkpoints[checkpoint_ind].size();

                test_out.AddMeasure("n_keys", n_keys);
                test_out.AddMeasure("aae", aae);
                test_out.AddMeasure("are", are);
                test_out.AddMeasure("total_overestimation", total_overestimation);
                test_out.AddMeasure("total_underestimation", total_underestimation);
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
                    timer_results['t'] = std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - time_points['t']).count();
                    aae /= total_count;
                    are /= total_count;
                    con /= total_count;

                    test_out.AddMeasure("top_aae", aae);
                    test_out.AddMeasure("top_are", are);
                }

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

                if (std::chrono::duration_cast<std::chrono::milliseconds>(timer::now() - op_start_time).count() 
                            > kill_exec_time_threshold)
                    return;
                break;
            }
        }
    }
}


argparse::ArgumentParser init_parser(const std::string& name) {
    argparse::ArgumentParser parser(name);

    parser.add_argument("arg")
            .help("the initial memory budget of the sketch, in bytes")
            .nargs(1)
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

    parser.add_argument("-r", "--rows")
            .help("the number of rows in the sketch")
            .nargs(1)
            .default_value(static_cast<uint32_t>(3))
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

