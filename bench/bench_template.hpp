#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <argparse/argparse.hpp>
#include <string>
#include <unordered_map>
#include "bench_utils.hpp"

#define start_timer(t) \
    auto t_start_##t = timer::now(); \

#define stop_timer(t) \
    auto t_end_##t = timer::now(); \
    test_out.add_measure(#t, std::chrono::duration_cast<std::chrono::milliseconds>(t_end_##t - t_start_##t).count());

TestOutput test_out = TestOutput();
bool test_verbose = true, print_csv = false;
std::string csv_file = "";

argparse::ArgumentParser init_parser(const std::string &name)
{
    argparse::ArgumentParser parser(name);

    parser.add_argument("arg")
            .help("the initial memory budget of the sketch, in bytes")
            .nargs(1)
            .scan<'i', int>();

    parser.add_argument("-r", "--rows")
            .help("the number of rows in the sketch")
            .nargs(1)
            .default_value(4)
            .scan<'i', int>();

    parser.add_argument("-w", "--workload")
            .help("pass the workload from file")
            .nargs(2, 3);

    parser.add_argument("-k", "--keys")
            .help("pass the keys from file")
            .nargs(1);

    parser.add_argument("--csv")
            .help("prints the output in csv")
            .nargs(1);

    return parser;
}

std::tuple<InputKeys<std::string>, int, int> read_parser_arguments(argparse::ArgumentParser &parser) 
{
    auto memory = parser.get<int>("arg");
    auto rows = parser.get<int>("rows");
    auto keys_filename = parser.get<std::string>("keys");
    uint32_t key_len_binary = 0;
    if (keys_filename == "CAIDA.dat")
        key_len_binary = 13;
    auto keys = read_data_binary(keys_filename, key_len_binary);

    if (keys.empty())
        throw std::runtime_error("error, keys file is empty.");

    if (auto arg_csv = parser.present<std::string>("--csv")) {
        print_csv = true;
        csv_file = *arg_csv;
    }

    std::cout << "[+] nkeys=" << keys.size() << std::endl;
    std::cout << "[+] Read keys, starting test." << std::endl;
    return std::make_tuple(keys, memory, rows);
}

std::unordered_map<std::string, uint32_t> actual_freq;

template<typename Sketch, typename InsertFun, typename QueryFun, typename SizeFun>
void experiment(Sketch &sketch, InsertFun insert_f, QueryFun query_f, SizeFun size_f, 
                InputKeys<std::string> &keys)
{
    for (auto &i : keys)
        actual_freq[i]++;

    start_timer(insert_time);
    int cnt = 0;
    for (auto &i : keys) {
        insert_f(sketch, i);
    }
    stop_timer(insert_time);
    std::cout << "[+] Keys inserted in " << test_out["insert_time"] << "ms, checking accuracy" << std::endl;

    double ARE = 0, AAE = 0, CON = 0;
    start_timer(query_time);
    for (auto &it : actual_freq) {
        const uint32_t est_val = query_f(sketch, it.first);
        const uint32_t real_val = it.second;
        const double dist = std::abs(static_cast<double>(est_val - real_val));

        ARE += dist / real_val;
        AAE += dist;
        CON += est_val != real_val;
    }
    stop_timer(query_time);
	ARE /= actual_freq.size();
    AAE /= actual_freq.size();
    CON /= actual_freq.size();
    std::cout << "[+] Queries processed in " << test_out["query_time"] << "ms" << std::endl;

    auto size = size_f(sketch);
    test_out.add_measure("size", size);
    test_out.add_measure("ARE", ARE);
    test_out.add_measure("AAE", AAE);
    test_out.add_measure("AAE", CON);
    std::cout << "[+] Test executed successfully, printing stats and closing." << std::endl;
}

void print_test() 
{
    if (test_verbose)
        test_out.print();

    if (print_csv) {
        std::cout << "[+] writing results in " << csv_file << std::endl;
        std::filesystem::path path_csv(csv_file);
        std::string s = (!std::filesystem::exists(path_csv) || std::filesystem::is_empty(path_csv))
                            ? test_out.to_csv(true) : test_out.to_csv(false);
        std::ofstream outFile(path_csv, std::ios::app);
        outFile << s;
        outFile.close();
    }
}

