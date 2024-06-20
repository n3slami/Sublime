#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#include "MurmurHash.hpp"
#include "util.hpp"

class CMSketchbookSemiAdaptiveCounters {
    friend class CMSketchbookSemiAdaptiveCountersTest;

public:
    CMSketchbookSemiAdaptiveCounters(size_t init_col_count, size_t init_row_count,
                                     std::function<uint64_t(size_t)> expansion_f,
                                     uint32_t seed_gen_seed)
                                     : row_count(init_row_count), init_col_count(init_col_count),
                                       expansion_f(expansion_f), seed_gen_seed(seed_gen_seed) {
        n = 0;
        col_count = init_col_count;
        expansion_lim = expansion_f(col_count);
        counter_width = highbit_pos(expansion_lim) + 1;
        contraction_lim = 0;
        init_col_count_lg = highbit_pos(init_col_count) + 1;
        col_count_lg = init_col_count_lg;

        sketches.push_back(allocate_sketch(row_count, col_count));
        gen_seeds();
    }
    
    // Should probably write a copy constructor...

    ~CMSketchbookSemiAdaptiveCounters() {
        for (uint8_t *ptr : sketches) {
            delete ptr;
        }
        sketches.clear();
        delete seeds;
    }

    void Insert(const uint64_t elem) {
        if (n == expansion_lim)
            expand();
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++) {
            const uint64_t val = get_counter(sketch, col_count * i + hash_key(elem, i));
            set_counter(sketch, col_count * i + hash_key(elem, i), val + 1);
        }
        n++;
    }

    void Delete(const uint64_t elem) {
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++) {
            const uint64_t val = get_counter(sketch, col_count * i + hash_key(elem, i));
            set_counter(sketch, col_count * i + hash_key(elem, i), val - 1);
        }
        n--;
        if (n == contraction_lim)
            contract();
    }

    uint64_t Query(const uint64_t elem) {
        uint64_t res = MAX_VALUE(8 * sizeof(uint32_t));
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++)
            res = std::min(res, get_counter(sketch, col_count * i + hash_key(elem, i)));
        return res;
    }


private:
    uint64_t n, counter_width;
    uint64_t expansion_lim, contraction_lim;
    uint32_t seed_gen_seed;
    uint32_t *seeds;
    size_t init_col_count, col_count;
    const size_t row_count;
    uint32_t init_col_count_lg, col_count_lg;
    std::function<uint64_t(size_t)> expansion_f;
    std::vector<uint8_t *> sketches;

    //__attribute__((always_inline))
    inline uint64_t get_counter(const uint8_t *sketch, const uint32_t pos, const uint32_t width) {
        const uint32_t bit_pos = pos * width;
        const uint64_t *p = reinterpret_cast<const uint64_t *>(sketch + bit_pos / 8);
        // you cannot just do *p to get the value, undefined behavior
        uint64_t pvalue;
        memcpy(&pvalue, p, sizeof(pvalue));
        return (uint64_t) ((pvalue >> (bit_pos % 8)) & BITMASK(width));
    }

    //__attribute__((always_inline))
    inline uint64_t get_counter(const uint8_t *sketch, const uint32_t pos) {
        return get_counter(sketch, pos, counter_width);
    }


    //__attribute__((always_inline))
    inline void set_counter(uint8_t *sketch, const uint32_t pos, const uint32_t width, const uint64_t value) {
        const uint32_t bit_pos = pos * width;
        uint64_t *p = reinterpret_cast<uint64_t *>(sketch + bit_pos / 8);
        // This is undefined:
        //uint64_t t = *p;
        uint64_t t;
        memcpy(&t, p, sizeof(t));
        uint64_t mask = BITMASK(width);
        uint64_t v = value;
        const uint32_t shift = bit_pos % 8;
        mask <<= shift;
        v <<= shift;
        t &= ~mask;
        t |= v;
        // this is undefined
        //*p = t;
        memcpy(p, &t, sizeof(t));
    }

    //__attribute__((always_inline))
    inline void set_counter(uint8_t *sketch, const uint32_t pos, const uint64_t value) {
        set_counter(sketch, pos, counter_width, value);
    }


    inline uint8_t *allocate_sketch(const size_t rows, const size_t cols) {
        const uint32_t size = (rows * cols * counter_width + 7) / 8;
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

    inline void expand() {
        contraction_lim = expansion_lim;
        expansion_lim = expansion_f(2 * col_count);
        const uint32_t old_counter_width = counter_width;
        counter_width = highbit_pos(expansion_lim) + 1;

        uint8_t *new_sketch = allocate_sketch(row_count, 2 * col_count);
        uint8_t *old_sketch = sketches.back();
        for (int i = 0; i < row_count; i++) {
            for (int j = 0; j < col_count; j++) {
                const uint32_t old_value = get_counter(old_sketch, col_count * i + j, old_counter_width);
                set_counter(new_sketch, 2 * col_count * i + j, counter_width, old_value);
                set_counter(new_sketch, 2 * col_count * i + col_count + j, counter_width, old_value);
            }
        }
        sketches.push_back(new_sketch);
        col_count *= 2;
        col_count_lg++;
    }

    inline void contract() {
        expansion_lim = contraction_lim;
        contraction_lim = (n < expansion_f(2 * init_col_count) ? 0 : expansion_f(col_count / 2));
        const uint32_t old_counter_width = counter_width;
        counter_width = highbit_pos(expansion_lim) + 1;

        uint8_t *new_sketch = sketches.back();
        sketches.pop_back();
        uint8_t *old_sketch = sketches.back();
        col_count /= 2;
        col_count_lg--;
        for (int i = 0; i < row_count; i++) {
            for (int j = 0; j < col_count; j++) {
                const uint32_t a = get_counter(new_sketch, 2 * col_count * i + j, old_counter_width);
                const uint32_t b = get_counter(new_sketch, 2 * col_count * i + col_count + j, old_counter_width);
                const uint32_t c = get_counter(old_sketch, col_count * i + j, counter_width);
                set_counter(old_sketch, col_count * i + j, counter_width, a + b - c);
            }
        }
        delete new_sketch;
    }
};

