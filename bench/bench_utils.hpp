#pragma once

#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <algorithm>
#include <iterator>
#include <random>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <string>

using timer = std::chrono::high_resolution_clock;

template<typename KeyType>
using Workload = std::vector<std::tuple<KeyType, KeyType, bool>>;

template<typename KeyType>
using InputKeys = std::vector<KeyType>;

std::vector<std::string> read_data_binary(const std::string &filename, const uint32_t length) 
{
    std::vector<std::string> data;
    std::fstream in(filename, std::ios::in | std::ios::binary);
    char key[length + 1];
    memset(key, 0, length + 1);
    while (true) {
        in.read(key, length);
        if (in.gcount() < length)
            break;
        data.emplace_back(std::string(key, length));
    }
    in.close();

    return data;
}

class TestOutput {
private:
    std::map<std::string, std::string> test_values;

public:
    template<typename TestValueType>
    inline void add_measure(const std::string &key, TestValueType value) {
        auto str = std::to_string(value);
        test_values[key] = str;
    }

    inline void add_measure(const std::string &key, const std::string &value) {
        test_values[key] = value;
    }

    inline void print() const {
        for (auto t: test_values) {
            std::cout << t.first << ": " << t.second << std::endl;
        }
    }

    auto operator[](const std::string &key) {
        return test_values[key];
    }

    std::string to_csv(bool print_header = true) {
        std::string s = "";

        if (print_header) {
            for (auto it = test_values.begin(); it != test_values.end(); ++it) {
                if (it != test_values.begin())
                    s += ",";
                s += (*it).first;
            }
            s += '\n';
        }

        for (auto it = test_values.begin(); it != test_values.end(); ++it) {
            if (it != test_values.begin())
                s += ",";
            s += (*it).second;
        }
        s += '\n';

        return s;
    }

};

