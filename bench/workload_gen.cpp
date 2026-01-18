/*
 * This file is part of Sketchbook <--->.
 * Copyright (C) 2025 ---.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "bench_utils.hpp"
#include "zipf/zipf.h"
#include <argparse/argparse.hpp>
#include <x86intrin.h>

static const std::vector<std::string> fdist_names = {"unif", "norm", "zipf", "real"};
static const std::vector<std::string> fdist_default = {"zipf"};

const uint64_t default_n_keys = 100'000'000;
const uint64_t default_universe_size = 100'000;
const uint64_t default_n_deletes = 100'000'000;
const uint64_t default_measurement_period = 1'000'000;

InputKeys<uint64_t> keys_from_file = InputKeys<uint64_t>();

const char pbstr[] = "||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||";
const size_t pbwidth = 60;
uint32_t seed = 2025;


void print_progress(double percentage) {
    static int last_percentage = 0;
    int val = (int) (percentage * 100);
    if (last_percentage == val)
        return;

    int lpad = static_cast<int>(percentage * pbwidth);
    int rpad = pbwidth - lpad;
    last_percentage = val;
    printf("\r%3d%% [%.*s%*s]", val, lpad, pbstr, rpad, "");
    fflush(stdout);
}

bool create_dir_recursive(const std::string_view& dir_name) {
    std::error_code err;
    if (!std::filesystem::create_directories(dir_name, err)) {
        if (std::filesystem::exists(dir_name))
            return true; // the folder probably already existed
        std::cerr << "Failed to create [" << dir_name << "]" << std::endl;
        return false;
    }
    return true;
}

std::vector<uint64_t> generate_int_keys_uniform(uint64_t n_keys, uint64_t universe_size, std::mt19937_64& rng) {
    std::vector<uint64_t> keys;
    std::uniform_int_distribution<uint64_t> dist(0, universe_size);
    std::cout << "Generating keys..." << std::endl;
    while (keys.size() < n_keys) {
        keys.push_back(dist(rng));
        print_progress(1.0 * keys.size() / n_keys);
    }
    std::cout << std::endl;
    return keys;
}

std::vector<uint64_t> generate_int_keys_normal(uint64_t n_keys, uint64_t universe_size, long double std, std::mt19937_64& rng) {
    std::vector<uint64_t> keys;
    std::normal_distribution<long double> dist(static_cast<double>(universe_size) / 2.0, std);
    std::cout << "Generating keys..." << std::endl;;
    while (keys.size() < n_keys) {
        const uint64_t value = std::clamp(static_cast<int64_t>(dist(rng)),
                                          0L, static_cast<int64_t>(universe_size));
        keys.push_back(value);
        print_progress(1.0 * keys.size() / n_keys);
    }
    std::cout << std::endl;
    return keys;
}

std::vector<uint64_t> generate_int_keys_zipf(uint64_t n_keys, uint64_t universe_size, long double char_exp, std::mt19937_64& rng) {
    std::vector<uint64_t> keys;
    keys.resize(n_keys);
    generate_random_keys(keys.data(), universe_size, n_keys, char_exp);
    return keys;
}


std::tuple<std::string, long double, long double, std::string> get_fdist(argparse::ArgumentParser& parser, uint32_t& pos) {
    std::string dist_name = parser.get<std::vector<std::string>>("--fdist")[pos++];
    std::string key_file;
    long double sigma = 0.0, char_exp = 0.0;
    if (std::find(fdist_names.begin(), fdist_names.end(), dist_name) == fdist_names.end()) {
        std::string msg = "Invalid key distribution name: ";
        msg += dist_name;
        throw std::runtime_error(msg);
    }

    if (dist_name == "norm")
        sigma = std::stod(parser.get<std::vector<std::string>>("--fdist")[pos++]);
    else if (dist_name == "zipf")
         char_exp = std::stod(parser.get<std::vector<std::string>>("--fdist")[pos++]);
    else if (dist_name == "real")
        key_file = parser.get<std::vector<std::string>>("--fdist")[pos++];
    pos = pos == parser.get<std::vector<std::string>>("--fdist").size() ? std::numeric_limits<uint32_t>::max() 
                                                                        : pos;
    return {dist_name, sigma, char_exp, key_file};
}


 ///////////////////////////////////////////////////////////////////////////// 
///////////////////////////////////////////////////////////////////////////////
////                        Benchmark Functions                            ////
///////////////////////////////////////////////////////////////////////////////
 ///////////////////////////////////////////////////////////////////////////// 


void standard_int_bench(argparse::ArgumentParser& parser) {
    WorkloadIO wio(parser.get<std::string>("--output-file"), WorkloadIO::iomode::Write, false);
    uint32_t fdist_ind = 0;
    auto [freq_dist, freq_dist_std, freq_dist_char_exp, key_file] = get_fdist(parser, fdist_ind);

    const uint32_t n_keys = parser.get<uint64_t>("--n-keys");
    const uint64_t universe_size = parser.get<uint64_t>("--universe-size");
    const uint64_t seed = parser.get<uint64_t>("--seed");
    std::mt19937_64 rng(seed);

    std::vector<uint64_t> keys;
    if (freq_dist == "unif")
        keys = generate_int_keys_uniform(n_keys, universe_size, rng);
    else if (freq_dist == "norm")
        keys = generate_int_keys_normal(n_keys, universe_size, freq_dist_std, rng);
    else if (freq_dist == "zipf")
        keys = generate_int_keys_zipf(n_keys, universe_size, freq_dist_char_exp, rng);
    else {
        if (parser.get<uint32_t>("--key-len-binary") > 0)
            keys = read_data_binary<uint64_t>(key_file);
        else
            keys = read_data_text(key_file);
    }

    std::set<uint64_t> key_set = {keys.begin(), keys.end()};
    std::unordered_map<uint64_t, uint64_t> alias;
    for (uint64_t key : key_set)
        alias[key] = rng();
    std::shuffle(keys.begin(), keys.end(), rng);

    wio.Timer('i');
    for (uint64_t key : keys)
        wio.Insert(alias[key]);
    wio.Timer('i');
    wio.Flush();
}


void standard_string_bench(argparse::ArgumentParser& parser) {
    WorkloadIO wio(parser.get<std::string>("--output-file"), WorkloadIO::iomode::Write, true);
    const uint32_t n_keys = parser.get<uint64_t>("--n-keys");
    const uint64_t universe_size = parser.get<uint64_t>("--universe-size");
    const uint64_t seed = parser.get<uint64_t>("--seed");
    std::mt19937_64 rng(seed);

    uint32_t fdist_ind = 0;
    std::vector<ByteString> keys;
    const uint32_t key_len_binary = parser.get<uint32_t>("--key-len-binary");
    while (fdist_ind != std::numeric_limits<uint32_t>::max()) {
        auto [freq_dist, freq_dist_std, freq_dist_char_exp, key_file] = get_fdist(parser, fdist_ind);

        std::vector<ByteString> new_keys;
        new_keys = read_data_binary(key_file, key_len_binary);
        keys.insert(keys.end(), new_keys.begin(), new_keys.end());
    }
    std::shuffle(keys.begin(), keys.end(), rng);

    wio.Timer('i');
    for (ByteString key : keys)
        wio.Insert(key);
    wio.Timer('i');
    wio.Flush();
}


void expand_bench(argparse::ArgumentParser& parser) {
    const uint32_t n_keys = parser.get<uint64_t>("--n-keys");
    const uint64_t universe_size = parser.get<uint64_t>("--universe-size");
    const uint64_t seed = parser.get<uint64_t>("--seed");
    std::mt19937_64 rng(seed);

    uint32_t fdist_ind = 0;
    std::vector<ByteString> keys;
    std::vector<uint64_t> int_keys;
    bool is_string = true;
    while (fdist_ind != std::numeric_limits<uint32_t>::max()) {
        auto [freq_dist, freq_dist_std, freq_dist_char_exp, key_file] = get_fdist(parser, fdist_ind);

        const uint32_t key_len_binary = parser.get<uint32_t>("--key-len-binary");
        if (key_len_binary > 0) {
            std::vector<ByteString> new_keys = read_data_binary(key_file, key_len_binary);
            keys.insert(keys.end(), new_keys.begin(), new_keys.end());
        }
        else {
            std::vector<uint64_t> new_keys = read_data_text(key_file);
            int_keys.insert(int_keys.end(), new_keys.begin(), new_keys.end());
            is_string = false;
        }
    }
    std::shuffle(keys.begin(), keys.end(), rng);
    std::shuffle(int_keys.begin(), int_keys.end(), rng);

    WorkloadIO wio(parser.get<std::string>("--output-file"), WorkloadIO::iomode::Write, is_string);
    const uint32_t measurement_period = parser.get<uint64_t>("--measurement-period");
    for (uint32_t i = 0; i + measurement_period <= std::max(keys.size(), int_keys.size()); i += measurement_period) {
        wio.Timer('i');
        for (uint32_t j = i; j < i + measurement_period; j++) {
            if (is_string)
                wio.Insert(keys[j]);
            else
                wio.Insert(int_keys[j]);
        }
        wio.Timer('i');
        wio.Flush();
    }
}


void delete_bench(argparse::ArgumentParser& parser) {
    WorkloadIO wio(parser.get<std::string>("--output-file"), WorkloadIO::iomode::Write, true);
    const uint32_t n_keys = parser.get<uint64_t>("--n-keys");
    const uint64_t universe_size = parser.get<uint64_t>("--universe-size");
    const uint64_t seed = parser.get<uint64_t>("--seed");
    std::mt19937_64 rng(seed);

    uint32_t fdist_ind = 0;
    std::vector<ByteString> keys;
    while (fdist_ind != std::numeric_limits<uint32_t>::max()) {
        auto [freq_dist, freq_dist_std, freq_dist_char_exp, key_file] = get_fdist(parser, fdist_ind);

        const uint32_t key_len_binary = parser.get<uint32_t>("--key-len-binary");
        std::vector<ByteString> new_keys = read_data_binary(key_file, key_len_binary);
        keys.insert(keys.end(), new_keys.begin(), new_keys.end());
    }
    std::shuffle(keys.begin(), keys.end(), rng);

    wio.Timer('i');
    for (ByteString key : keys)
        wio.Insert(key);
    wio.Timer('i');
    wio.Flush();

    //std::shuffle(keys.begin(), keys.end(), rng);
    const uint32_t n_deletes = std::min(keys.size(), parser.get<uint64_t>("--n-deletes"));
    const uint32_t measurement_period = parser.get<uint64_t>("--measurement-period");
    for (uint32_t i = 0; i < std::min<uint32_t>(keys.size(), n_deletes); i += measurement_period) {
        wio.Timer('d');
        for (uint32_t j = i; j < std::min<uint32_t>(keys.size(), i + measurement_period); j++)
            wio.Delete(keys[keys.size() - j - 1]);
        wio.Timer('d');
        wio.Flush();
    }
}


void join_size_bench(argparse::ArgumentParser& parser) {
    WorkloadIO wio(parser.get<std::string>("--output-file"), WorkloadIO::iomode::Write, true);
    const uint64_t seed = parser.get<uint64_t>("--seed");
    std::mt19937_64 rng(seed);

    uint32_t fdist_ind = 0;
    int32_t table_ind = 0;
    std::vector<ByteString> keys;
    std::vector<uint32_t> max_repeats = parser.get<std::vector<uint32_t>>("--max-repeat");
    while (fdist_ind != std::numeric_limits<uint32_t>::max()) {
        wio.SwitchTable(table_ind);
        if (table_ind > max_repeats.size())
            throw std::runtime_error("Must have exactly one max repeat value for each distribution.");
        auto [freq_dist, freq_dist_std, freq_dist_char_exp, key_file] = get_fdist(parser, fdist_ind);

        const std::string tpc_h_table_check = ".tbl";
        if (freq_dist != "real" 
                || key_file.compare(key_file.length() - tpc_h_table_check.length(),
                    tpc_h_table_check.length(),
                    tpc_h_table_check) != 0)
            throw std::runtime_error("Join size estimation experiment must use the TPC-H dataset.");

        for (std::string& key : read_data_text_string(key_file)) {
            const ByteString key_byte_str {reinterpret_cast<const uint8_t *>(key.data()), 
                static_cast<uint32_t>(key.size())};
            for (uint32_t i = 0; i <= rng() % max_repeats[table_ind]; i++)
                wio.Insert(key_byte_str);
        }
        table_ind++;
    }
    wio.Flush();
}


std::unordered_map<std::string, std::function<void(argparse::ArgumentParser&)>> benches = {
    {"standard", standard_int_bench},
    {"standard_string", standard_string_bench},
    {"expand", expand_bench},
    {"delete", delete_bench},
    {"join_size", join_size_bench},
};

int main(int argc, char const *argv[]) {
    argparse::ArgumentParser parser("workload_gen");

    {
        std::string msg = "The benchmark type to create [";
        bool first_bench = true;
        for (auto bench : benches) {
            if (first_bench)
                msg += bench.first;
            else
                msg += " | " + bench.first;
            first_bench = false;
        }
        msg += "]";
        parser.add_argument("-t", "--type")
                .help(msg)
                .required()
                .nargs(1);
    }

    parser.add_argument("-o", "--output-file")
            .help("The path to the output file")
            .required()
            .nargs(1);

    parser.add_argument("--fdist")
            .help("The (possibly multiple, for different phases) frequency distributions")
            .nargs(argparse::nargs_pattern::at_least_one)
            .required()
            .default_value(fdist_default);

    parser.add_argument("-n", "--n-keys")
            .help("The number of keys in the input stream")
            .required()
            .default_value(static_cast<uint64_t>(default_n_keys))
            .scan<'u', uint64_t>()
            .nargs(1);

    parser.add_argument("-u", "--universe-size")
            .help("The size of the universe")
            .required()
            .default_value(static_cast<uint64_t>(default_universe_size))
            .scan<'u', uint64_t>()
            .nargs(1);


    parser.add_argument("-d", "--n-deletes")
            .help("The number of delete operations in the input stream")
            .required()
            .default_value(static_cast<uint64_t>(default_n_deletes))
            .scan<'u', uint64_t>()
            .nargs(1);

    parser.add_argument("-m", "--measurement-period")
            .help("The period in which accuracy measurement should be done")
            .required()
            .default_value(static_cast<uint64_t>(default_measurement_period))
            .scan<'u', uint64_t>()
            .nargs(1);

    parser.add_argument("--key-len-binary")
            .help("The length of the keys in the binary file if they all share the same length, in bytes")
            .required()
            .default_value(static_cast<uint32_t>(13))
            .scan<'u', uint32_t>()
            .nargs(1);

    parser.add_argument("--seed")
            .help("The seed used for random number generation")
            .required()
            .default_value(1380UL)
            .scan<'u', uint64_t>()
            .nargs(1);

    parser.add_argument("--max-repeat")
            .help("The number of times each key in the corresponding distribution should be repeated in join size estimation")
            .nargs(argparse::nargs_pattern::at_least_one)
            .scan<'u', uint32_t>()
            .default_value(1);

    try {
        parser.parse_args(argc, argv);
    }
    catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << parser;
        std::exit(1);
    }

    std::string bench_type = parser.get<std::string>("--type");
    if (benches.find(bench_type) != benches.end())
        benches[bench_type](parser);
    else {
        std::string msg = "Error: Invalid benchmark type. Valid benchmarks: ";
        for (auto bench : benches)
            msg += bench.first + " ";
        throw std::runtime_error(msg);
    }

    return 0;
}
