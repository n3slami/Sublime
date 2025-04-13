#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#include "MurmurHash.hpp"
#include "util.hpp"

template<typename T>
class CMSketchbookFixedCounters {
    friend class CMSketchbookFixedCountersTest;

public:
    CMSketchbookFixedCounters(size_t init_col_count, size_t init_row_count,
                              std::function<uint64_t(size_t)> expansion_f,
                              uint32_t seed_gen_seed)
                              : row_count(init_row_count), init_col_count(init_col_count),
                                expansion_f(expansion_f), seed_gen_seed(seed_gen_seed) {
        n = 0;
        col_count = init_col_count;
        expansion_lim = expansion_f(col_count);
        contraction_lim = 0;
        init_col_count_lg = highbit_pos(init_col_count) + 1;
        col_count_lg = init_col_count_lg;

        sketches.push_back(allocate_sketch(row_count, col_count));
        gen_seeds();
    }
    
    // Should probably write a copy constructor...

    ~CMSketchbookFixedCounters() {
        for (uint8_t *ptr : sketches) {
            delete ptr;
        }
        sketches.clear();
        delete seeds;
    }

    void Insert(const uint64_t elem) {
        if (n == expansion_lim)
            expand();
        T * sketch = reinterpret_cast<T *>(sketches.back());
        for (int i = 0; i < row_count; i++)
            sketch[col_count * i + hash_key(elem, i)]++;
        n++;
    }

    void Insert(const char *elem, const uint32_t length) {
        if (n == expansion_lim)
            expand();
        T * sketch = reinterpret_cast<T *>(sketches.back());
        for (int i = 0; i < row_count; i++)
            sketch[col_count * i + hash_string_key(elem, length, i)]++;
        n++;
    }

    void Delete(const uint64_t elem) {
        T * sketch = reinterpret_cast<T *>(sketches.back());
        for (int i = 0; i < row_count; i++)
            sketch[col_count * i + hash_key(elem, i)]--;
        n--;
        if (n == contraction_lim)
            contract();
    }

    void Delete(const char *elem, const uint32_t length) {
        T * sketch = reinterpret_cast<T *>(sketches.back());
        for (int i = 0; i < row_count; i++)
            sketch[col_count * i + hash_string_key(elem, length, i)]--;
        n--;
        if (n == contraction_lim)
            contract();
    }


    T Query(const uint64_t elem) {
        T res = MAX_VALUE(8 * sizeof(T) - 1);
        T * sketch = reinterpret_cast<T *>(sketches.back());
        for (int i = 0; i < row_count; i++)
            res = std::min(res, sketch[col_count * i + hash_key(elem, i)]);
        return res;
    }

    T Query(const char *elem, const uint32_t length) {
        T res = MAX_VALUE(8 * sizeof(T) - 1);
        T * sketch = reinterpret_cast<T *>(sketches.back());
        for (int i = 0; i < row_count; i++)
            res = std::min(res, sketch[col_count * i + hash_string_key(elem, length, i)]);
        return res;
    }

private:
    uint64_t n;
    uint64_t expansion_lim, contraction_lim;
    uint32_t seed_gen_seed;
    uint32_t *seeds;
    size_t init_col_count, col_count;
    const size_t row_count;
    uint32_t init_col_count_lg, col_count_lg;
    std::function<uint64_t(size_t)> expansion_f;
    std::vector<uint8_t *> sketches;

    inline uint8_t *allocate_sketch(const size_t rows, const size_t cols) {
        const uint32_t size = rows * cols * sizeof(T);
        uint8_t *res = new uint8_t[size];
        memset(res, 0, size);
        return res;
    }

    inline void gen_seeds() {
        std::mt19937 rng(seed_gen_seed);
        seeds = new uint32_t[row_count];
        for (int i = 0; i < row_count; i++)
            seeds[i] = rng();
    }

    inline uint32_t hash_key(const uint64_t key, const int seed_ind) {
        const uint64_t original_hash = MurmurHash64B(&key, sizeof(key), seeds[seed_ind]);
        uint32_t hash = original_hash & BITMASK(init_col_count_lg);
        hash = fast_reduce(hash << (8 * sizeof(uint32_t) - init_col_count_lg),
                            init_col_count);
        hash += ((original_hash >> init_col_count_lg) & BITMASK(col_count_lg - init_col_count_lg))
                * init_col_count;
        return hash;
    }

    inline uint32_t hash_string_key(const char *key, const uint32_t length, const int seed_ind) {
        const uint64_t original_hash = MurmurHash64B(key, length, seeds[seed_ind]);
        uint32_t hash = original_hash & BITMASK(init_col_count_lg);
        hash = fast_reduce(hash << (8 * sizeof(uint32_t) - init_col_count_lg),
                            init_col_count);
        hash += ((original_hash >> init_col_count_lg) & BITMASK(col_count_lg - init_col_count_lg))
                * init_col_count;
        return hash;
    }

    inline void expand() {
        contraction_lim = expansion_lim;
        expansion_lim = expansion_f(2 * col_count);

        T *new_sketch = reinterpret_cast<T *>(allocate_sketch(row_count, 2 * col_count));
        T *old_sketch = reinterpret_cast<T *>(sketches.back());
        for (int i = 0; i < row_count; i++) {
            for (int j = 0; j < col_count; j++) {
                new_sketch[2 * col_count * i + j] = old_sketch[col_count * i + j];
                new_sketch[2 * col_count * i + col_count + j] = old_sketch[col_count * i + j];
            }
        }
        sketches.push_back(reinterpret_cast<uint8_t *>(new_sketch));
        col_count *= 2;
        col_count_lg++;
    }

    inline void contract() {
        expansion_lim = contraction_lim;
        contraction_lim = (n < expansion_f(2 * init_col_count) ? 0 : expansion_f(col_count / 2));

        T *new_sketch = reinterpret_cast<T *>(sketches.back());
        sketches.pop_back();
        T *old_sketch = reinterpret_cast<T *>(sketches.back());
        col_count /= 2;
        col_count_lg--;
        for (int i = 0; i < row_count; i++) {
            for (int j = 0; j < col_count; j++) {
                old_sketch[col_count * i + j] = new_sketch[2 * col_count * i + j]
                                                + new_sketch[2 * col_count * i + col_count + j]
                                                - old_sketch[col_count * i + j];
            }
        }
        delete new_sketch;
    }
};

