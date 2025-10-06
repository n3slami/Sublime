/*
 * This file is part of Sketchbook <https://github.com/n3slami/Sketchbook>.
 * Copyright (C) 2025 Navid Eslami
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

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <ostream>
#include <random>
#include <vector>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <cstring>
#include <string_view>

/**
 * This file contains some utility functions and data structures used in the benchmarks.
 */
#define TO_MB(x) (x / (1024.0 * 1024.0))
#define TO_BPK(x, n) ((double)(x * 8) / n)

inline std::string uint64ToString(uint64_t key) {
    uint64_t endian_swapped_key = __builtin_bswap64(key);
    return std::string(reinterpret_cast<const char *>(&endian_swapped_key), 8);
}

inline uint64_t stringToUint64(const std::string_view str_key) {
    uint64_t int_key = 0;
    memcpy(reinterpret_cast<char *>(&int_key), str_key.data(), 8);
    return __builtin_bswap64(int_key);
}

inline bool has_suffix(const std::string_view str, const std::string_view suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const uint64_t string_rng_seed = 1000;
static std::mt19937_64 string_rng(string_rng_seed);
static inline void generate_random_string(uint8_t *str, uint32_t len) {
    uint32_t i;
    uint64_t *word_str = reinterpret_cast<uint64_t *>(str);
    for (i = 0; i + 8 <= len; i += 8)
        word_str[i / 8] = std::max<uint64_t>(string_rng(), 1ULL);
    for (; i < len; i++)
        str[i] = std::max<uint8_t>(string_rng() % 256, 1U);
    str[len] = '\0';
}

using timer = std::chrono::high_resolution_clock;


struct ByteString {
    uint8_t *str;
    uint16_t length;

    ByteString(const uint8_t *buf, uint32_t buf_len)
        : length(buf_len) {
        str = new uint8_t[buf_len];
        memcpy(str, buf, buf_len);
    }

    ByteString(const ByteString& other)
        : length(other.length) {
        str = new uint8_t[other.length];
        memcpy(str, other.str, other.length);
    }

    ByteString(ByteString&& other)
        : length(other.length) {
        str = other.str;
        other.str = nullptr;
    }

    ~ByteString() {
        delete[] str;
    }

    ByteString& operator=(const ByteString& other) {
        delete[] str;
        length = other.length;
        str = new uint8_t[other.length];
        memcpy(str, other.str, other.length);
        return *this;
    }

    bool operator<(const ByteString& other) const {
        uint32_t min_length = std::min(length, other.length);
        int32_t cmp = memcmp(str, other.str, min_length);
        if (cmp == 0)
            return length < other.length;
        return cmp < 0;
    }

    bool operator<=(const ByteString& other) const {
        uint32_t min_length = std::min(length, other.length);
        int32_t cmp = memcmp(str, other.str, min_length);
        if (cmp == 0)
            return length <= other.length;
        return cmp < 0;
    }

    bool operator>(const ByteString& other) const {
        return !(*this <= other);
    }

    bool operator>=(const ByteString& other) const {
        return !(*this < other);
    }
};

inline std::ostream& operator<<(std::ostream& os, const ByteString& s)
{
    for (int32_t i = 0; i < s.length; i++) {
        for (int32_t j = 7; j >= 0; j--)
            os << ((s.str[i] >> j) & 1);
        os << ' ';
    }
    return os;
}


inline uint32_t calculate_lcp(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len) {
    uint32_t min_len = std::min(a_len, b_len);
    for (int32_t i = 0; i < min_len; i++)
        if (a[i] != b[i])
            return i;
    return min_len;
}


template<typename KeyType>
using InputKeys = std::vector<KeyType>;

template <typename KeyType>
std::vector<KeyType> read_data_binary(const std::string& filename) {
    std::vector<KeyType> data;
    std::fstream in(filename, std::ios::in | std::ios::binary);
    in.exceptions(std::ios::failbit | std::ios::badbit);
    KeyType size;
    in.read((char *) &size, sizeof(KeyType));
    data.resize(size);
    in.read((char *) data.data(), size * sizeof(KeyType));
    return data;
}

inline std::vector<ByteString> read_data_binary(const std::string &filename, uint16_t length) {
    std::vector<ByteString> data;
    std::fstream in(filename, std::ios::in | std::ios::binary);
    uint8_t key[length + 1];
    memset(key, 0, length + 1);
    while (true) {
        in.read(reinterpret_cast<char *>(key), length);
        if (in.gcount() < length)
            break;
        data.push_back({key, length});
    }
    in.close();

    return data;
}

inline std::vector<ByteString> read_data_binary(const std::string& filename) {
    std::vector<ByteString> data;
    std::fstream in(filename, std::ios::in | std::ios::binary);
    in.exceptions(std::ios::failbit | std::ios::badbit);
    uint8_t buf[std::numeric_limits<uint16_t>::max()];
    uint16_t key_size;
    while (!in.eof()) {
        in.read(reinterpret_cast<char *>(&key_size), sizeof(key_size));
        in.read(reinterpret_cast<char *>(buf), key_size);
        data.push_back({buf, key_size});
    }
    return data;
}

inline std::vector<uint64_t> read_data_text(const std::string &filename) {
    std::vector<uint64_t> data;
    std::fstream in(filename, std::ios::in);
    uint64_t key;
    while (in >> key)
        data.emplace_back(key);
    in.close();
    return data;
}


class WorkloadIO {
public:
    enum class opcode {
        Insert,
        Delete,
        Timer,
        Flush
    };

    enum class iomode {
        Read,
        Write
    };


    WorkloadIO() { }

    WorkloadIO(std::string_view file_path, iomode mode, bool string_keys=false)
            : mode_(mode),
              string_keys_(string_keys) {
        if (mode == iomode::Read) {
            buf_size_ = std::filesystem::file_size(file_path);
            buf_ = new uint8_t[buf_size_];
            io_ = std::fstream(file_path.data(), std::ios::in | std::ios::binary);
            io_.read(reinterpret_cast<char *>(buf_), buf_size_);
            string_keys_ = buf_[head_++];
        }
        else {
            io_ = std::fstream(file_path.data(), std::ios::out | std::ios::binary);
            io_.write(reinterpret_cast<const char *>(&string_keys_), sizeof(string_keys_));
        }
    }

    WorkloadIO& operator=(const WorkloadIO& other) {
        if (mode_ == iomode::Write)
            throw std::runtime_error("Cannot copy a WorkloadIO in write mode");
        string_keys_ = other.string_keys_;
        delete[] buf_;
        buf_ = new uint8_t[other.buf_size_];
        memcpy(buf_, other.buf_, other.buf_size_);
        buf_size_ = other.buf_size_;
        head_ = other.head_;
        return *this;
    }

    ~WorkloadIO() {
        delete[] buf_;
        io_.close();
    }


    template <typename T>
    void WriteValue(T value) {
        io_.write(reinterpret_cast<const char *>(&value), sizeof(value));
    }

    void WriteOpcode(opcode value) {
        const uint8_t value_byte = static_cast<uint8_t>(value);
        io_.write(reinterpret_cast<const char *>(&value_byte), 1);
    }

    void WriteByteString(ByteString& s) {
        WriteValue(s.length);
        io_.write(reinterpret_cast<const char *>(s.str), s.length);
    }

    template <typename T>
    T ReadValue() {
        T value;
        memcpy(&value, buf_ + head_, sizeof(value));
        head_ += sizeof(value);
        return value;
    }


    void Insert(ByteString& key) {
        WriteOpcode(opcode::Insert);
        WriteByteString(key);
    }

    void Insert(uint64_t key) {
        WriteOpcode(opcode::Insert);
        WriteValue(key);
    }


    void Delete(ByteString& key) {
        WriteOpcode(opcode::Delete);
        WriteByteString(key);
    }

    void Delete(uint64_t key) {
        WriteOpcode(opcode::Delete);
        WriteValue(key);
    }


    void Timer(char timer_stamp) {
        WriteOpcode(opcode::Timer);
        WriteValue(timer_stamp);
    }


    void Flush() {
        WriteOpcode(opcode::Flush);
    }


    bool Done() const {
        return mode_ == iomode::Write ? false : head_ >= buf_size_;
    }

    void Reset() {
        head_ = 0;
        string_keys_ = buf_[head_++];
    }


    opcode GetOpcode() {
        return static_cast<opcode>(buf_[head_++]);
    }

    void GetStringKey(uint16_t& length, uint8_t *key) {
        length = ReadValue<uint16_t>();
        memcpy(key, buf_ + head_, length);
        head_ += length;
    }

    bool StringKeys() const {
        return string_keys_;
    }

private:
    std::fstream io_;
    iomode mode_;
    bool string_keys_;
    uint8_t *buf_ = nullptr;
    uint64_t head_ = 0;
    size_t buf_size_ = 0;
};


class TestOutput {
    std::map<std::string, std::string> test_values;

public:
    template<typename TestValueType>
    void AddMeasure(const std::string& key, TestValueType value) {
        std::string str = std::to_string(value);
        if (key == "fpr" && str == "0.000000")
            str = "0.000001";
        test_values[key] = str;
    }

    void AddMeasure(const std::string& key, const std::string& value) {
        test_values[key] = value;
    }

    void Clear() {
        test_values.clear();
    }

    void Print() const {
        for (auto t: test_values) {
            std::cout << t.first << ": " << t.second << std::endl;
        }
    }

    auto operator[](const std::string& key) {
        return test_values[key];
    }

    std::string ToJson() {
        std::string res = "{\n";
        for (auto it = test_values.begin(); it != test_values.end(); ++it) {
            res += "\t\"";
            res += (*it).first;
            res += "\": ";
            res += (*it).second;
            res += ",\n";
        }
        res += "}\n";
        return res;
    }
};
