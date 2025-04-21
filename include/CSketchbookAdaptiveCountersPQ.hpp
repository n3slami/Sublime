#pragma once

#include <algorithm>
#include <bits/floatn-common.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#include "MurmurHash.hpp"
#include "util.hpp"

class CSketchbookAdaptiveCountersPQ {
    friend class CSketchbookAdaptiveCountersTest;

public:
    CSketchbookAdaptiveCountersPQ(size_t init_col_count, size_t init_row_count,
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

        counter_count = row_count * col_count;
        for (uint32_t i = 0; i < prefetch_queue_len; i++) {
            prefetch_queue[i] = new uint64_t[row_count];
            prefetch_queue_cache_line[i] = new uint64_t[row_count];
            prefetch_queue_cache_line_offset[i] = new uint64_t[row_count];
            prefetch_queue_sign[i] = new int64_t[row_count];
            for (uint32_t j = 0; j < row_count; j++) {
                prefetch_queue[i][j] = counter_count + 1000;
                prefetch_queue_cache_line[i][j] = get_cache_line_ind(counter_count + 1000);
            }
            memset(prefetch_queue_cache_line_offset[i], 0, sizeof(uint64_t) * row_count);
            memset(prefetch_queue_sign[i], 0, sizeof(int64_t) * row_count);
        }
		bias_range = std::min(static_cast<uint32_t>(((64 - ceil(log2(counter_count)))) / (row_count - 1)), 8U);
        bias_mask = BITMASK(bias_range);
        index_range	= std::min(static_cast<uint32_t>(64 - bias_range * row_count),
                               static_cast<uint32_t>(ceil(log2(counter_count)) - bias_range));
		index_mask = BITMASK(index_range);

        sketches.push_back(allocate_sketch(row_count, col_count));
        gen_seeds();

        setup_lookup_tables();
    }
    
    // Should probably write a copy constructor...
    CSketchbookAdaptiveCountersPQ(const CSketchbookAdaptiveCountersPQ&) = delete;
    CSketchbookAdaptiveCountersPQ& operator=(const CSketchbookAdaptiveCountersPQ&) = delete;

    ~CSketchbookAdaptiveCountersPQ() {
        for (uint32_t i = 0; i < sketches.size(); i++) {
            // Delete the inner large arrays, if any
            const uint32_t cols = col_count >> (sketches.size() - i - 1);
            const uint32_t cache_line_cnt = (row_count * cols + counter_per_cache_line - 1) / counter_per_cache_line;
            for (uint32_t j = 0; j < cache_line_cnt; j++) {
                const uint64_t *words = reinterpret_cast<uint64_t *>(sketches[i] + j * cache_line_size_bytes);
                if (words[cache_line_size_words - 2] >> 63) {
                    delete reinterpret_cast<uint32_t *>(words[cache_line_size_words - 1]);
                }
            }
            delete sketches[i];
        }
        for (uint32_t i = 0; i < prefetch_queue_len; i++)
            delete prefetch_queue[i];
        sketches.clear();
        delete seeds;
    }

    void Insert(const uint64_t elem) {
        if (n == expansion_lim)
            expand();
        uint8_t *sketch = sketches.back();
        const uint32_t old_prefetch_clock = prefetch_clock;
        prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
        uint64_t sign_hash = get_sign_hash(elem);
        for (int i = 0; i < row_count; i++) {
            const uint32_t pos = col_count * i + hash_key(elem, i);
            __builtin_prefetch(sketch + get_cache_line_ind(pos) * cache_line_size_bytes);
            prefetch_queue[old_prefetch_clock][i] = pos;
            prefetch_queue_sign[old_prefetch_clock][i] = sign_hash & 1;
            incdec_counter(sketch,
                           prefetch_queue[prefetch_clock][i],
                           prefetch_queue_sign[prefetch_clock][i]);
            sign_hash >>= 1;
        }
        n++;
    }

    void Insert(const char *elem, const uint32_t length) {
        if (n == expansion_lim)
            expand();
        uint8_t *sketch = sketches.back();
        const uint32_t old_prefetch_clock = prefetch_clock;
        prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
        uint64_t sign_hash = get_string_sign_hash(elem, length);
        for (int i = 0; i < row_count; i++) {
            const uint32_t pos = col_count * i + hash_string_key(elem, length, i);
            __builtin_prefetch(sketch + get_cache_line_ind(pos) * cache_line_size_bytes);
            prefetch_queue[old_prefetch_clock][i] = pos;
            prefetch_queue_sign[old_prefetch_clock][i] = sign_hash & 1ULL;
            incdec_counter(sketch,
                           prefetch_queue[prefetch_clock][i],
                           prefetch_queue_sign[prefetch_clock][i]);
            sign_hash >>= 1;
        }
        n++;
    }

    void InsertTofHashing(const char *elem, const uint32_t length) {
        if (n == expansion_lim)
            expand();
        uint8_t *sketch = sketches.back();
        uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
		uint32_t index = ((hash_value & index_mask) << bias_range);
		int32_t tmp = index - counter_count;
		index = (tmp < 0 ? index : tmp);
        hash_value >>= index_range;

        const uint32_t old_prefetch_clock = prefetch_clock;
        prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
        for (int i = 0; i < row_count; i++) {
            /* Old prefetching
            prefetch_queue[old_prefetch_clock][i] = index + (i << bias_range) + (hash_value & bias_mask);
            __builtin_prefetch(sketch + get_cache_line_ind(prefetch_queue[old_prefetch_clock][i]) * cache_line_size_bytes);
            hash_value >>= bias_range;
            increment_counter(sketch, prefetch_queue[prefetch_clock][i]);
            */
            const uint32_t counter_pos = index + (i << bias_range) + (hash_value & bias_mask);
            prefetch_queue_cache_line[old_prefetch_clock][i] = get_cache_line_ind(counter_pos);
            prefetch_queue_cache_line_offset[old_prefetch_clock][i] = counter_pos - counter_per_cache_line 
                                                                                    * prefetch_queue_cache_line[old_prefetch_clock][i];
            __builtin_prefetch(sketch + prefetch_queue_cache_line[old_prefetch_clock][i] * cache_line_size_bytes);
            hash_value >>= bias_range;
            prefetch_queue_sign[old_prefetch_clock][i] = hash_value & 1ULL;
            hash_value >>= 1;
            const uint64_t tmp_pos = prefetch_queue_cache_line[prefetch_clock][i] * counter_per_cache_line 
                                   + prefetch_queue_cache_line_offset[prefetch_clock][i];
            incdec_counter(sketch,
                           prefetch_queue_cache_line[prefetch_clock][i],
                           prefetch_queue_cache_line_offset[prefetch_clock][i],
                           prefetch_queue_sign[prefetch_clock][i]);
        }
        n++;
    }

    void Delete(const uint64_t elem) {
        uint8_t *sketch = sketches.back();
        uint64_t sign_hash = ~get_sign_hash(elem);
        for (int i = 0; i < row_count; i++) {
            incdec_counter(sketch, col_count * i + hash_key(elem, i), sign_hash & 1ULL);
            sign_hash >>= 1;
        }
        n--;
        if (n == contraction_lim)
            contract();
    }

    void Delete(const char *elem, const uint32_t length) {
        uint8_t *sketch = sketches.back();
        uint64_t sign_hash = ~get_string_sign_hash(elem, length);
        for (int i = 0; i < row_count; i++) {
            incdec_counter(sketch, col_count * i + hash_string_key(elem, length, i), sign_hash & 1ULL);
            sign_hash >>= 1;
        }
        n--;
        if (n == contraction_lim)
            contract();
    }

    void DeleteTofHashing(const char *elem, const uint32_t length) {
        uint8_t *sketch = sketches.back();
        uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
		uint32_t index = ((hash_value & index_mask) << bias_range);
		int32_t tmp = index - counter_count;
		index = (tmp < 0 ? index : tmp);
        hash_value >>= index_range;

        const uint32_t old_prefetch_clock = prefetch_clock;
        prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
        for (int i = 0; i < row_count; i++) {
            /* Old prefetching
            prefetch_queue[old_prefetch_clock][i] = index + (i << bias_range) + (hash_value & bias_mask);
            __builtin_prefetch(sketch + get_cache_line_ind(prefetch_queue[old_prefetch_clock][i]) * cache_line_size_bytes);
            hash_value >>= bias_range;
            increment_counter(sketch, prefetch_queue[prefetch_clock][i]);
            */
            const uint32_t counter_pos = index + (i << bias_range) + (hash_value & bias_mask);
            prefetch_queue_cache_line[old_prefetch_clock][i] = get_cache_line_ind(counter_pos);
            prefetch_queue_cache_line_offset[old_prefetch_clock][i] = counter_pos - counter_per_cache_line 
                                                                                    * prefetch_queue_cache_line[old_prefetch_clock][i];
            __builtin_prefetch(sketch + prefetch_queue_cache_line[old_prefetch_clock][i] * cache_line_size_bytes);
            hash_value >>= bias_range;
            prefetch_queue_sign[old_prefetch_clock][i] = hash_value & 1ULL;
            hash_value >>= 1;
            incdec_counter(sketch,
                           prefetch_queue_cache_line[prefetch_clock][i],
                           prefetch_queue_cache_line_offset[prefetch_clock][i],
                           prefetch_queue_sign[prefetch_clock][i] ^ 1ULL);
        }
        n--;
        if (n == contraction_lim)
            contract();
    }

    int64_t Query(const uint64_t elem) const {
        int64_t res[row_count];
        const uint8_t *sketch = sketches.back();
        uint64_t sign_hash = get_sign_hash(elem);
        for (int i = 0; i < row_count; i++) {
            const int64_t raw_val = get_counter(sketch, col_count * i + hash_key(elem, i));
            res[i] = (sign_hash & 1ULL) ? raw_val : -raw_val;
            sign_hash >>= 1;
        }
        std::sort(res, res + row_count);
        return res[(row_count - 1) / 2];
    }

    int64_t Query(const char *elem, const uint32_t length) const {
        int64_t res[row_count];
        const uint8_t *sketch = sketches.back();
        uint64_t sign_hash = get_string_sign_hash(elem, length);
        for (int i = 0; i < row_count; i++) {
            const int64_t raw_val = get_counter(sketch, col_count * i + hash_string_key(elem, length, i));
            res[i] = (sign_hash & 1ULL) ? raw_val : -raw_val;
            sign_hash >>= 1;
        }
        std::sort(res, res + row_count);
        return res[(row_count - 1) / 2];
    }

    uint64_t QueryTofHashing(const char *elem, const uint32_t length) {
        uint64_t res[row_count];
        const uint8_t *sketch = sketches.back();
        uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
		uint32_t index = ((hash_value & index_mask) << bias_range);
		int32_t tmp = index - counter_count;
		index = (tmp < 0 ? index : tmp);
        hash_value >>= index_range;
        for (int i = 0; i < row_count; i++) {
            const uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
            const int64_t raw_val = get_counter(sketch, pos);
            hash_value >>= bias_range;
            res[i] = (hash_value & 1ULL) ? raw_val : -raw_val;
            hash_value >>= 1;
        }
        std::sort(res, res + row_count);
        return res[(row_count - 1) / 2];
    }

    size_t Size() {
        const uint32_t cache_line_cnt = (row_count * col_count + counter_per_cache_line - 1) / counter_per_cache_line;
        const size_t base_size = cache_line_cnt * cache_line_size_bytes;
        const uint8_t *sketch = sketches.back();
        const uint32_t cache_line_count = (row_count * col_count + counter_per_cache_line - 1) / counter_per_cache_line;
        const uint32_t sep_1 = counter_per_cache_line;
        const uint32_t sep_2 = sep_1 + counter_per_cache_line * base_counter_size;
        const uint32_t sep_3 = sep_2 + second_word_bit_count;
        uint32_t extra_arrays = 0;
        for (int i = 0; i < cache_line_count; i++) {
            const uint8_t *ptr = sketch + i * cache_line_size_bytes;
            const uint64_t *words = reinterpret_cast<const uint64_t *>(ptr);
            if (words[cache_line_size_words - 2] >> 63)
                extra_arrays++;
        }
        return base_size + extra_arrays * sizeof(uint32_t) * counter_per_cache_line;
    }


private:
    uint64_t n;
    uint64_t expansion_lim, contraction_lim;
    uint32_t seed_gen_seed, sign_seed;
    uint32_t *seeds;
    size_t init_col_count, col_count, counter_count;
    const size_t row_count;
    uint32_t init_col_count_lg, col_count_lg;
    std::function<uint64_t(size_t)> expansion_f;
    std::vector<uint8_t *> sketches;

    // Stingy Tofs
    static const uint64_t prefetch_queue_len = 16;
    uint64_t *prefetch_queue[prefetch_queue_len];
    uint64_t *prefetch_queue_cache_line[prefetch_queue_len];
    uint64_t *prefetch_queue_cache_line_offset[prefetch_queue_len];
    int64_t *prefetch_queue_sign[prefetch_queue_len];
    uint32_t prefetch_clock = 0;
    uint32_t bias_range, index_range;
    uint64_t bias_mask, index_mask;

    static const uint64_t select_mask = 0x5555555555555555;
    static const uint32_t cache_line_size = 512, counter_per_cache_line = 63;
    static const uint32_t cache_line_size_bytes = cache_line_size / 8;
    static const uint32_t cache_line_size_words = cache_line_size / (8 * sizeof(uint64_t));
    static const uint32_t base_counter_size = 6, extension_size = 2;
    static const uint32_t num_extension = (cache_line_size - 1 - counter_per_cache_line * (base_counter_size + 1)) / extension_size;
    static const uint64_t second_word_bit_count = cache_line_size - counter_per_cache_line * (base_counter_size + 1) - 64;
    uint8_t word_update_byte_offset[64], word_update_shamt[64];

    void setup_lookup_tables() {
        const uint32_t offset_granule = sizeof(uint64_t) / 2 * 8;
        uint32_t bit_pos = counter_per_cache_line;
        uint32_t l = offset_granule, r = offset_granule + sizeof(uint64_t) * 8;
        for (int i = 0; i < counter_per_cache_line; i++) {
            const uint32_t counter_pos = i * base_counter_size + counter_per_cache_line;
            if (counter_pos + base_counter_size > r) {
                l += offset_granule;
                r += offset_granule;
            }
            word_update_byte_offset[i] = l / 8;
            word_update_shamt[i] = counter_pos - l;
        }
    }

    static_assert((8 * sizeof(uint64_t)) % extension_size == 0,
                  "Word size must be divisible by the extension size, at least for now");

    //__attribute__((always_inline))
    inline uint32_t get_cache_line_ind(const uint32_t pos) const {
        return static_cast<int32_t>(pos) / counter_per_cache_line;
    }

    //__attribute__((always_inline))
    inline uint64_t get_extension_mask(const uint64_t val) const {
        return (val & (val >> 1)) & select_mask;
    }

    //__attribute__((always_inline))
    inline uint32_t get_extension_pos(const uint64_t extensions[], const uint32_t rank) const {
        const uint64_t masks[2] = {get_extension_mask(extensions[0]), get_extension_mask(extensions[1])};
        int32_t extension_pos = (rank == 0 ? -2 : bit_select(masks[0], rank - 1));
        const int32_t other_attempt = bit_select(masks[1], rank - 1 - __builtin_popcountll(masks[0]));
        extension_pos = (extension_pos >= 64 ? other_attempt + 64 : extension_pos);
        return extension_pos + 2;
    }

    //__attribute__((always_inline))
    inline uint32_t get_extension_length(uint64_t extensions[]) const {
        const uint32_t a = highbit_pos(extensions[1]);
        const uint32_t b = highbit_pos(extensions[0]);
        return (a ? a + 64 : b) + 1;
    }

    //__attribute__((always_inline))
    inline void shift_extensions_left_from_pos(uint64_t extensions[], const uint32_t pos, const uint32_t shamt) const {
        if (pos < 64) {
            const uint64_t a = extensions[0] & BITMASK(pos);
            const uint64_t b = extensions[0] >> pos;
            extensions[1] <<= shamt;
            extensions[1] |= b >> (64 - pos - shamt);
            extensions[0] &= BITMASK(pos);
            extensions[0] |= (b << (pos + shamt));
        }
        else {
            const uint32_t new_pos = pos - 64;
            const uint64_t b = extensions[1] & (~BITMASK(new_pos));
            extensions[1] &= BITMASK(new_pos);
            extensions[1] |= b << shamt;
        }
    }

    //__attribute__((always_inline))
    inline void shift_extensions_right_from_pos(uint64_t extensions[], const uint32_t pos, const uint32_t shamt) const {
        if (pos < 64) {
            const uint64_t a = extensions[1] & BITMASK(shamt);
            const uint64_t b = extensions[0] >> (pos + shamt);
            extensions[1] >>= shamt;
            extensions[0] &= BITMASK(pos);
            extensions[0] |= ((a << (64 - shamt)) | (b << pos));
        }
        else {
            const uint32_t new_pos = pos - 64;
            const uint64_t a = extensions[1] & (~BITMASK(new_pos + shamt));
            extensions[1] &= BITMASK(new_pos);
            extensions[1] |= a >> shamt;
        }
    }

    //__attribute__((always_inline))
    inline int64_t get_counter(const uint8_t *sketch, const uint32_t pos) const {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        const uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Calculate the base counter
        int64_t res;
        const uint32_t base_bit_pos = inter_cache_line_ind * base_counter_size + counter_per_cache_line;
        const uint32_t base_byte_pos = base_bit_pos / 8;
        memcpy(&res, cache_line_ptr + base_byte_pos, sizeof(res));
        const int64_t non_masked_res = res >> (base_bit_pos % 8);
        const int64_t sign = (non_masked_res >> (base_counter_size - 1)) & 1ULL;
        res = non_masked_res & BITMASK(base_counter_size - 1);

        // Take into account the extensions, if any
        const uint64_t *words = reinterpret_cast<const uint64_t *>(cache_line_ptr);
        const bool has_extension = (words[0] >> inter_cache_line_ind) & 1;
        if (has_extension) {
            const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
            uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                            words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
            // Check for pointers
            if ((extension_bitmap[1] >> (second_word_bit_count - 1)) & 1) {
                const uint32_t *ptr = reinterpret_cast<const uint32_t *>(extension_bitmap[0]);
                res |= ptr[inter_cache_line_ind] << (base_counter_size - 1);
            }
            else {
                uint64_t add_res = 0, add_pw = 1;
                for (int i = get_extension_pos(extension_bitmap, extension_rank); true; i += extension_size) {
                    const uint32_t new_bits = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                    if (new_bits == MAX_VALUE(extension_size))
                        break;
                    add_res += new_bits * add_pw;
                    add_pw *= MAX_VALUE(extension_size);
                }
                res += add_res << (base_counter_size - 1);
            }
        }

        return sign ? -res : res;
    }

    //__attribute__((always_inline))
    inline uint32_t *setup_separate_array(const uint64_t has_extension_bitmap, uint64_t *extension_bitmap,
                                          const uint32_t total_extension_len) {
        uint32_t *ptr = new uint32_t[counter_per_cache_line];
        memset(ptr, 0, counter_per_cache_line * sizeof(uint32_t));
        uint32_t running_val = 0, running_pw = 1, cnt = 0;
        for (int i = 0; i < total_extension_len; i += extension_size) {
            const uint32_t fragment = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
            if (fragment == 3) {
                ptr[bit_select(has_extension_bitmap, cnt)] = running_val;
                running_val = 0;
                running_pw = 1;
                cnt++;
                continue;
            }
            running_val = running_val + running_pw * fragment;
            running_pw *= 3;
        }
        extension_bitmap[0] = reinterpret_cast<uint64_t>(ptr);
        extension_bitmap[1] |= 1ULL << (second_word_bit_count - 1);
        return ptr;
    }

    //__attribute__((always_inline))
    inline void write_extensions_to_cache_line(uint64_t *words, uint64_t *extension_bitmap) {
        extension_bitmap[1] = (extension_bitmap[1] << (64 - second_word_bit_count))
                                | (words[cache_line_size_words - 2] & BITMASK(64 - second_word_bit_count));
        words[cache_line_size_words - 2] = extension_bitmap[1];
        words[cache_line_size_words - 1] = extension_bitmap[0];
    }

    //__attribute__((always_inline))
    inline void set_counter(uint8_t *sketch, const uint32_t pos, const int64_t _value) {
        const int64_t sign = _value < 0;
        const int64_t value = _value < 0 ? -_value : _value;

        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        uint64_t stamp;
        const uint32_t base_bit_pos = inter_cache_line_ind * base_counter_size + counter_per_cache_line;
        const uint32_t base_byte_pos = base_bit_pos / 8;
        memcpy(&stamp, cache_line_ptr + base_byte_pos, sizeof(stamp));
        stamp &= (~(BITMASK(base_counter_size) << (base_bit_pos % 8)));
        const uint64_t update_val = ((sign << (base_counter_size - 1)) 
                                    | (value & BITMASK(base_counter_size - 1)));
        stamp |= update_val << (base_bit_pos % 8);
        memcpy(cache_line_ptr + base_byte_pos, &stamp, sizeof(stamp));
        
        // Handle the extensions
        uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
        const bool new_has_extension = value > MAX_VALUE(base_counter_size - 1);
        const bool old_has_extension = (words[0] >> inter_cache_line_ind) & 1;
        const int update_state = static_cast<int>(old_has_extension) * 2 + static_cast<int>(new_has_extension);
        if (update_state) {
            const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
            uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                            words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
            const bool already_has_array_ptr = extension_bitmap[1] >> (second_word_bit_count - 1);
            const uint32_t total_extension_len = (already_has_array_ptr ? 2 * num_extension + 1
                                                                        : get_extension_length(extension_bitmap));
            uint32_t new_extension_len = extension_size, extension_value = value >> (base_counter_size - 1);
            for (uint64_t val = extension_value; val; val /= MAX_VALUE(extension_size))
                new_extension_len += extension_size;

            switch (update_state) {
                case 1: {   // !old_has_extension && new_has_extension
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else if (new_extension_len + total_extension_len > extension_size * num_extension) {
                        uint32_t *ptr = setup_separate_array(words[0], extension_bitmap, total_extension_len);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else {
                        // Handle Locally
                        const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                        shift_extensions_left_from_pos(extension_bitmap, pos, new_extension_len);
                        uint64_t tmp_val = extension_value, i;
                        for (i = pos; tmp_val; tmp_val /= MAX_VALUE(extension_size), i += extension_size)
                            extension_bitmap[i / 64] |= ((tmp_val % MAX_VALUE(extension_size)) << (i % 64));
                        extension_bitmap[i / 64] |= (MAX_VALUE(extension_size) << (i % 64));
                    }
                    break;
                }
                case 2: {   // old_has_extension && !new_has_extension
                    // Handle Locally
                    const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                    const uint32_t old_extension_len = get_extension_pos(extension_bitmap, extension_rank + 1) - pos;
                    shift_extensions_right_from_pos(extension_bitmap, pos, old_extension_len);
                    break;
                }
                case 3: {   // old_has_extension && new_has_extension
                    const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                    const uint32_t old_extension_len = get_extension_pos(extension_bitmap, extension_rank + 1) - pos;
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else if (new_extension_len + total_extension_len - old_extension_len > extension_size * num_extension) {
                        uint32_t *ptr = setup_separate_array(words[0], extension_bitmap, total_extension_len);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else {
                        // Handle Locally
                        shift_extensions_right_from_pos(extension_bitmap, pos, old_extension_len);
                        shift_extensions_left_from_pos(extension_bitmap, pos, new_extension_len);
                        uint64_t tmp_val = extension_value, i;
                        for (i = pos; tmp_val; tmp_val /= MAX_VALUE(extension_size), i += extension_size)
                            extension_bitmap[i / 64] |= ((tmp_val % MAX_VALUE(extension_size)) << (i % 64));
                        extension_bitmap[i / 64] |= (MAX_VALUE(extension_size) << (i % 64));
                    }
                    break;
                }
            }

            write_extensions_to_cache_line(words, extension_bitmap);
        }
        words[0] = (new_has_extension ? words[0] | (1ULL << inter_cache_line_ind)
                                      : words[0] & (~BITMASK(1ULL << inter_cache_line_ind)));
    }

    inline void incdec_counter(uint8_t *sketch, const uint32_t pos, const int32_t inc) {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        int64_t stamp;
        const uint32_t base_bit_pos = inter_cache_line_ind * base_counter_size + counter_per_cache_line;
        const uint32_t base_byte_pos = base_bit_pos / 8;
        memcpy(&stamp, cache_line_ptr + base_byte_pos, sizeof(stamp));
        const int64_t non_masked_val = stamp >> (base_bit_pos % 8);
        const int64_t val_sign = (non_masked_val >> (base_counter_size - 1)) & 1ULL;
        int64_t val = non_masked_val & BITMASK(base_counter_size - 1);

        const int64_t op = val_sign ^ inc;
        if (op) {
            const uint64_t carried = (val == MAX_VALUE(base_counter_size - 1));
            stamp += (carried ^ 1ULL) << (base_bit_pos % 8);
            stamp &= ~((BITMASK(base_counter_size - 1) << (base_bit_pos % 8)) & (-carried));
            memcpy(cache_line_ptr + base_byte_pos, &stamp, sizeof(stamp));

            // Handle the carry and extensions
            uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
            const bool has_extension = (words[0] >> inter_cache_line_ind) & 1;
            const int update_state = static_cast<int>(has_extension) * 2 + carried;
            if (carried) {
                const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
                uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                                words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
                const bool already_has_array_ptr = extension_bitmap[1] >> (second_word_bit_count - 1);
                const uint32_t total_extension_len = (already_has_array_ptr ? 2 * num_extension + 1
                        : get_extension_length(extension_bitmap));
                if (has_extension) {
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                        ptr[inter_cache_line_ind]++;
                    }
                    else {
                        const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                        uint32_t i = pos;
                        uint64_t chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                        bool absorbed = false, should_add_extension = true;
                        do {
                            absorbed = (chunk != MAX_VALUE(extension_size) - 1);
                            should_add_extension &= !absorbed;
                            extension_bitmap[i / 64] += (1ULL + (absorbed ? 0 : -MAX_VALUE(extension_size))) << (i % 64);
                            i += 2;
                            chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                        } while ((!absorbed) & (chunk != MAX_VALUE(extension_size)));

                        if (should_add_extension) {
                            if (total_extension_len + extension_size > extension_size * num_extension) {
                                uint32_t *ptr = setup_separate_array(words[0], extension_bitmap, total_extension_len);
                                ptr[inter_cache_line_ind] = 1;
                                for (int j = pos; j < i; j += 2)
                                    ptr[inter_cache_line_ind] *= 3;
                            }
                            else {
                                shift_extensions_left_from_pos(extension_bitmap, i, extension_size);
                                extension_bitmap[i / 64] += 1ULL << (i % 64);
                            }
                        }
                    }
                }
                else {
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                        ptr[inter_cache_line_ind]++;
                    }
                    else if (total_extension_len + 2 * extension_size > extension_size * num_extension) {
                        uint32_t *ptr = setup_separate_array(words[0], extension_bitmap, total_extension_len);
                        ptr[inter_cache_line_ind] = 1;
                    }
                    else {
                        const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                        shift_extensions_left_from_pos(extension_bitmap, pos, 2 * extension_size);
                        extension_bitmap[pos / 64] += 1ULL << (pos % 64);
                        extension_bitmap[(pos + 2) / 64] |= MAX_VALUE(extension_size) << ((pos + 2) % 64);
                    }
                }

                write_extensions_to_cache_line(words, extension_bitmap);
            }
            words[0] |= (carried << inter_cache_line_ind);
        }
        else {
            const uint64_t carried = (val == 0);
            stamp -= (carried ^ 1ULL) << (base_bit_pos % 8);
            uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
            const bool has_extension = (words[0] >> inter_cache_line_ind) & 1;
            if ((!has_extension) & carried) {
                stamp ^= ((1LL << (base_counter_size - 1)) | 1LL) << (base_bit_pos % 8);
                memcpy(cache_line_ptr + base_byte_pos, &stamp, sizeof(stamp));
                return;
            }
            stamp |= ((BITMASK(base_counter_size - 1) << (base_bit_pos % 8)) & (-carried));
            memcpy(cache_line_ptr + base_byte_pos, &stamp, sizeof(stamp));

            // Handle the carry and extensions
            const int update_state = static_cast<int>(has_extension) * 2 + carried;
            if (carried) {
                const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
                uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                                words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
                const bool already_has_array_ptr = extension_bitmap[1] >> (second_word_bit_count - 1);
                const uint32_t total_extension_len = (already_has_array_ptr ? 2 * num_extension + 1
                                                                            : get_extension_length(extension_bitmap));

                if (already_has_array_ptr) {
                    // Update separate array
                    uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                    ptr[inter_cache_line_ind]--;
                    const uint64_t extensions_depleted = ptr[inter_cache_line_ind] == 0;
                    words[0] &= ~(extensions_depleted << inter_cache_line_ind);
                }
                else {
                    const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                    uint32_t i = pos;
                    uint64_t chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                    bool absorbed = false, should_remove_extension = true;
                    do {
                        absorbed = (chunk != 0);
                        should_remove_extension &= (!absorbed | (chunk == 1));
                        extension_bitmap[i / 64] -= (1ULL + (absorbed ? 0 : -MAX_VALUE(extension_size))) << (i % 64);
                        i += 2;
                        chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                    } while ((!absorbed) & (chunk != MAX_VALUE(extension_size)));

                    should_remove_extension &= (chunk == MAX_VALUE(extension_size));
                    if (should_remove_extension) {
                        const uint64_t extensions_depleted = (i == pos + 2);
                        const uint32_t shamt = (extensions_depleted ? extension_size * 2 : extension_size);
                        const uint32_t shift_pos = i - extensions_depleted * 2 - 2;
                        shift_extensions_right_from_pos(extension_bitmap, shift_pos, shamt);
                        words[0] &= ~(extensions_depleted << inter_cache_line_ind);
                    }
                }

                write_extensions_to_cache_line(words, extension_bitmap);
            }
        }
    }

    inline void incdec_counter(uint8_t *sketch, const uint32_t cache_line_ind, const uint32_t inter_cache_line_ind,
                               const int32_t inc) {
        const uint64_t tmp_pos = cache_line_ind * counter_per_cache_line + inter_cache_line_ind;
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        uint64_t *update_word = reinterpret_cast<uint64_t *>(cache_line_ptr + word_update_byte_offset[inter_cache_line_ind]);
        const uint64_t base_counter_mask = BITMASK(base_counter_size - 1) << word_update_shamt[inter_cache_line_ind];
        const int64_t val = (update_word[0] & base_counter_mask);
        const int64_t val_sign = (update_word[0] >> (word_update_shamt[inter_cache_line_ind] + base_counter_size - 1))
                                    & 1LL;
        const int64_t op = val_sign ^ inc;
        if (op) {
            if (val != base_counter_mask) {
                update_word[0] += 1ULL << word_update_shamt[inter_cache_line_ind];
                return;
            }

            update_word[0] &= ~base_counter_mask;

            // Handle the carry and extensions
            uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
            const bool has_extension = (words[0] >> inter_cache_line_ind) & 1;
            const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
            uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                            words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
            const bool already_has_array_ptr = extension_bitmap[1] >> (second_word_bit_count - 1);
            const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * num_extension + 1
                    : get_extension_length(extension_bitmap));
            if (has_extension) {
                if (already_has_array_ptr) {
                    // Update separate array
                    uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                    ptr[inter_cache_line_ind]++;
                }
                else {
                    const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                    uint32_t i = pos;
                    uint64_t chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                    bool absorbed = false, should_add_extension = true;
                    do {
                        absorbed = (chunk != MAX_VALUE(extension_size) - 1);
                        should_add_extension &= !absorbed;
                        extension_bitmap[i / 64] += (1ULL + (absorbed ? 0 : -MAX_VALUE(extension_size))) << (i % 64);
                        i += 2;
                        chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                    } while ((!absorbed) & (chunk != MAX_VALUE(extension_size)));

                    if (should_add_extension) {
                        if (total_extension_len + extension_size > extension_size * num_extension) {
                            uint32_t *ptr = setup_separate_array(words[0], extension_bitmap, total_extension_len);
                            ptr[inter_cache_line_ind] = 1;
                            for (int j = pos; j < i; j += 2)
                                ptr[inter_cache_line_ind] *= 3;
                        }
                        else {
                            shift_extensions_left_from_pos(extension_bitmap, i, extension_size);
                            extension_bitmap[i / 64] += 1ULL << (i % 64);
                        }
                    }
                }
            }
            else {
                if (already_has_array_ptr) {
                    // Update separate array
                    uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                    ptr[inter_cache_line_ind]++;
                }
                else if (total_extension_len + 2 * extension_size > extension_size * num_extension) {
                    uint32_t *ptr = setup_separate_array(words[0], extension_bitmap, total_extension_len);
                    ptr[inter_cache_line_ind] = 1;
                }
                else {
                    const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                    shift_extensions_left_from_pos(extension_bitmap, pos, 2 * extension_size);
                    extension_bitmap[pos / 64] += 1ULL << (pos % 64);
                    extension_bitmap[(pos + 2) / 64] |= MAX_VALUE(extension_size) << ((pos + 2) % 64);
                }
            }

            write_extensions_to_cache_line(words, extension_bitmap);
            words[0] |= (1ULL << inter_cache_line_ind);
        }
        else {
            if (val != 0) {
                update_word[0] -= 1ULL << word_update_shamt[inter_cache_line_ind];
                return;
            }

            uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
            const bool has_extension = (words[0] >> inter_cache_line_ind) & 1;
            if (!has_extension) {
                update_word[0] ^= ((1LL << (base_counter_size - 1)) | 1LL) << word_update_shamt[inter_cache_line_ind];
                return;
            }
            update_word[0] |= base_counter_mask;

            // Handle the carry and extensions
            const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
            uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                            words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
            const bool already_has_array_ptr = extension_bitmap[1] >> (second_word_bit_count - 1);
            const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * num_extension + 1
                                                                        : get_extension_length(extension_bitmap));

            if (already_has_array_ptr) {
                // Update separate array
                uint32_t *ptr = reinterpret_cast<uint32_t *>(extension_bitmap[0]);
                ptr[inter_cache_line_ind]--;
                const uint64_t extensions_depleted = ptr[inter_cache_line_ind] == 0;
                words[0] &= ~(extensions_depleted << inter_cache_line_ind);
            }
            else {
                const uint32_t pos = get_extension_pos(extension_bitmap, extension_rank);
                uint32_t i = pos;
                uint64_t chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                bool absorbed = false, should_remove_extension = true;
                do {
                    absorbed = (chunk != 0);
                    should_remove_extension &= (!absorbed | (chunk == 1));
                    extension_bitmap[i / 64] -= (1ULL + (absorbed ? 0 : -MAX_VALUE(extension_size))) << (i % 64);
                    i += 2;
                    chunk = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                } while ((!absorbed) & (chunk != MAX_VALUE(extension_size)));

                should_remove_extension &= (chunk == MAX_VALUE(extension_size));
                if (should_remove_extension) {
                    const uint64_t extensions_depleted = (i == pos + 2);
                    const uint32_t shamt = (extensions_depleted ? extension_size * 2 : extension_size);
                    const uint32_t shift_pos = i - 2;
                    shift_extensions_right_from_pos(extension_bitmap, shift_pos, shamt);
                    words[0] &= ~(extensions_depleted << inter_cache_line_ind);
                }
            }

            write_extensions_to_cache_line(words, extension_bitmap);
        }
    }

    inline void increment_counter(uint8_t *sketch, const uint32_t pos) {
        incdec_counter(sketch, pos, 1);
    }

    inline void decrement_counter(uint8_t *sketch, const uint32_t pos) {
        incdec_counter(sketch, pos, 0);
    }

    inline uint8_t *allocate_sketch(const size_t rows, const size_t cols) {
        const uint32_t cache_line_cnt = 1000 + (rows * cols + counter_per_cache_line - 1) / counter_per_cache_line;
        const uint32_t size = cache_line_cnt * cache_line_size_bytes;
        uint8_t *res = new uint8_t[size];
        memset(res, 0, size);
        return res;
    }

    inline void gen_seeds() {
        seeds = new uint32_t[row_count];
        std::mt19937 rng(seed_gen_seed);
        for (int i = 0; i < row_count; i++)
            seeds[i] = rng();
        sign_seed = rng();
    }

    inline uint32_t hash_key(const uint64_t key, const int seed_ind) const {
        const uint64_t original_hash = MurmurHash64B(&key, sizeof(key), seeds[seed_ind]);
        uint32_t hash = original_hash & BITMASK(init_col_count_lg);
        hash = fast_reduce(hash << (8 * sizeof(uint32_t) - init_col_count_lg),
                            init_col_count);
        hash += ((original_hash >> init_col_count_lg) & BITMASK(col_count_lg - init_col_count_lg))
                * init_col_count;
        return hash;
    }

    inline uint32_t hash_string_key(const char *key, const uint32_t length, const int seed_ind) const {
        const uint64_t original_hash = MurmurHash64B(key, length, seeds[seed_ind]);
        uint32_t hash = original_hash & BITMASK(init_col_count_lg);
        hash = fast_reduce(hash << (8 * sizeof(uint32_t) - init_col_count_lg),
                            init_col_count);
        hash += ((original_hash >> init_col_count_lg) & BITMASK(col_count_lg - init_col_count_lg))
                * init_col_count;
        return hash;
    }

    inline uint64_t get_sign_hash(const uint64_t key) const {
        return MurmurHash64B(&key, sizeof(key), sign_seed);
    }

    inline uint64_t get_string_sign_hash(const char *key, const uint32_t length) const {
        return MurmurHash64B(key, length, sign_seed);
    }

    inline void expand() {
        contraction_lim = expansion_lim;
        expansion_lim = expansion_f(2 * col_count);

        uint8_t *new_sketch = allocate_sketch(row_count, 2 * col_count);
        uint8_t *old_sketch = sketches.back();
        for (int i = 0; i < row_count; i++) {
            for (int j = 0; j < col_count; j++) {
                const uint32_t old_value = get_counter(old_sketch, col_count * i + j);
                set_counter(new_sketch, 2 * col_count * i + j, old_value);
                set_counter(new_sketch, 2 * col_count * i + col_count + j, old_value);
            }
        }
        sketches.push_back(new_sketch);
        col_count *= 2;
        col_count_lg++;
    }

    inline void contract() {
        expansion_lim = contraction_lim;
        contraction_lim = (n < expansion_f(2 * init_col_count) ? 0 : expansion_f(col_count / 2));

        uint8_t *new_sketch = sketches.back();
        sketches.pop_back();
        uint8_t *old_sketch = sketches.back();
        col_count /= 2;
        col_count_lg--;
        for (int i = 0; i < row_count; i++) {
            for (int j = 0; j < col_count; j++) {
                const uint32_t a = get_counter(new_sketch, 2 * col_count * i + j);
                const uint32_t b = get_counter(new_sketch, 2 * col_count * i + col_count + j);
                const uint32_t c = get_counter(old_sketch, col_count * i + j);
                set_counter(old_sketch, col_count * i + j, a + b - c);
            }
        }
        delete new_sketch;
    }
};

