#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#include "MurmurHash.hpp"
#include "util.hpp"

template<typename T>
class CMS {
    friend class CMSTest;
    friend class SublimeCMSTest;

public:
    CMS(size_t init_col_count, size_t init_row_count, std::function<uint64_t(double)> expansion_f, uint32_t seed_gen_seed,
            bool use_tof_hashing=false)
                              : row_count(init_row_count), 
                                init_col_count(init_col_count),
                                expansion_f(expansion_f),
                                seed_gen_seed(seed_gen_seed),
                                using_tof_hashing(use_tof_hashing) {
        n = 0;
        col_count = init_col_count;
        expansion_lim = expansion_f(col_count);
        contraction_lim = expansion_f(col_count / 2.0);
        init_col_count_lg = highbit_pos(init_col_count) + (__builtin_popcountll(init_col_count) > 1);
        col_count_lg = init_col_count_lg;
        init_counter_count = row_count * col_count;
        if (using_tof_hashing) {
            if (init_counter_count - (1ULL << highbit_pos(init_counter_count)) < row_count)
                init_counter_count = (1ULL << highbit_pos(init_counter_count));
        }
        counter_count = init_counter_count;
        init_counter_count_lg = highbit_pos(counter_count) + (__builtin_popcountll(counter_count) > 1);
        counter_count_lg = init_counter_count_lg;

        // Setup Tof Hashing
		bias_range = std::min(static_cast<uint32_t>(((64 - counter_count_lg)) / row_count), 8U);
        bias_mask = BITMASK(bias_range);
        index_range	= std::min(static_cast<uint32_t>(64 - bias_range * row_count),
                               static_cast<uint32_t>(counter_count_lg - bias_range));
		index_mask = BITMASK(index_range);

        sketches.push_back(allocate_sketch(row_count, col_count));
        gen_seeds();
    }
    
    // Should probably write a copy constructor...
    CMS(const CMS&) = delete;
    CMS& operator= (const CMS&) = delete;

    ~CMS() {
        for (uint8_t *ptr : sketches) {
            delete[] ptr;
        }
        sketches.clear();
        delete[] seeds;
    }

    void Insert(const uint64_t elem) {
        Insert(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }

    void Insert(const char *elem, const uint32_t length) {
        if (n == expansion_lim)
            expand();
        T * sketch = reinterpret_cast<T *>(sketches.back());
        if (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            const uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                sketch[pos]++;
                hash_value >>= bias_range;
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                const uint32_t pos = col_count * i + hash_key(elem, length, i);
                assert(pos < col_count * row_count);
                sketch[pos]++;
            }
        }
        n++;
    }

    void Delete(const uint64_t elem) {
        Delete(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }

    void Delete(const char *elem, const uint32_t length) {
        T * sketch = reinterpret_cast<T *>(sketches.back());
        if (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            const uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                sketch[pos]--;
                hash_value >>= bias_range;
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                const uint32_t pos = col_count * i + hash_key(elem, length, i);
                assert(pos < col_count * row_count);
                sketch[pos]--;
            }
        }

        n--;
        if (n < contraction_lim)
            contract();
    }


    T Query(const uint64_t elem) const {
        return Query(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }

    T Query(const char *elem, const uint32_t length) const {
        T res = MAX_VALUE(8 * sizeof(T) - 1);
        T * sketch = reinterpret_cast<T *>(sketches.back());

        if (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            const uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                res = std::min(res, sketch[pos]);
                hash_value >>= bias_range;
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                const uint32_t pos = col_count * i + hash_key(elem, length, i);
                assert(pos < col_count * row_count);
                res = std::min(res, sketch[pos]);
            }
        }
        return res;
    }

    size_t Size() const {
        return col_count * row_count * sizeof(T);
    }

private:
    static constexpr uint64_t contraction_min_size = 1 << 3;

    uint64_t n;
    uint64_t expansion_lim, contraction_lim;
    uint32_t seed_gen_seed;
    uint32_t *seeds;
    size_t init_col_count, col_count, init_counter_count, counter_count;
    const size_t row_count;
    uint32_t init_col_count_lg, col_count_lg, init_counter_count_lg, counter_count_lg;
    std::function<uint64_t(size_t)> expansion_f;
    std::vector<uint8_t *> sketches;

    // Tof Hashing Stuff
    const bool using_tof_hashing = false;
    uint32_t bias_range, index_range;
    uint64_t bias_mask, index_mask;

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

    inline uint32_t hash_key(const char *key, const uint32_t length, const int seed_ind) const {
        const uint64_t original_hash = MurmurHash64B(key, length, seeds[seed_ind]);
        uint32_t hash = original_hash & BITMASK(init_col_count_lg);
        hash = fast_reduce(hash << (8 * sizeof(uint32_t) - init_col_count_lg),
                            init_col_count);
        hash += ((original_hash >> init_col_count_lg) & BITMASK(col_count_lg - init_col_count_lg))
                * init_col_count;
        return hash;
    }

    inline uint32_t hash_tof(const uint64_t original_hash) const {
        const int32_t hash_shamt = counter_count_lg - init_counter_count_lg;
        uint32_t hash = (original_hash & index_mask) << bias_range;
        int32_t tmp = hash - init_counter_count;
        hash = (tmp < 0 ? hash : tmp);
        if (hash_shamt >= 0)
            hash += ((original_hash >> (init_counter_count_lg + bias_range * row_count)) & BITMASK(hash_shamt))
                * init_counter_count;
        else
            hash &= (index_mask >> (-hash_shamt)) << bias_range;
        return hash;
    }


    inline void expand() {
        contraction_lim = expansion_lim;
        expansion_lim = expansion_f(2 * col_count);

        T *new_sketch = reinterpret_cast<T *>(allocate_sketch(row_count, 2 * col_count));
        T *old_sketch = reinterpret_cast<T *>(sketches.back());
        if (using_tof_hashing) {
            for (int64_t i = 0; i < counter_count; i++) {
                new_sketch[i] = old_sketch[i];
                new_sketch[counter_count + i] = old_sketch[i];
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                for (int j = 0; j < col_count; j++) {
                    new_sketch[2 * col_count * i + j] = old_sketch[col_count * i + j];
                    new_sketch[2 * col_count * i + col_count + j] = old_sketch[col_count * i + j];
                }
            }
        }
        sketches.push_back(reinterpret_cast<uint8_t *>(new_sketch));
        col_count *= 2;
        col_count_lg++;
        counter_count *= 2;
        counter_count_lg++;
    }

    inline void contract() {
        expansion_lim = contraction_lim;
        contraction_lim = (col_count / 4 < contraction_min_size ? 0 : expansion_f(col_count / 4));

        T *new_sketch = reinterpret_cast<T *>(sketches.back());
        sketches.pop_back();
        T *old_sketch = reinterpret_cast<T *>(sketches.back());
        col_count /= 2;
        col_count_lg--;
        counter_count /= 2;
        counter_count_lg--;
        if (using_tof_hashing) {
            for (int64_t i = 0; i < counter_count; i++)
                old_sketch[i] = new_sketch[i] + new_sketch[counter_count + i] - old_sketch[i];
        }
        else {
            for (int64_t i = 0; i < row_count; i++) {
                for (int64_t j = 0; j < col_count; j++) {
                    old_sketch[col_count * i + j] = new_sketch[2 * col_count * i + j]
                                                    + new_sketch[2 * col_count * i + col_count + j]
                                                    - old_sketch[col_count * i + j];
                }
            }
        }
        delete[] new_sketch;
    }
};

