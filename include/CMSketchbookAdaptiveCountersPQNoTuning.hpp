#pragma once

#include <bits/floatn-common.h>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <vector>

#include "MurmurHash.hpp"
#include "util.hpp"

class CMSketchbookAdaptiveCountersPQ {
    friend class CMSketchbookAdaptiveCountersTest;

private:
    enum class OpType {
        Insert,
        Delete,
        Query,
        None
    };

    struct Sketch {
        uint8_t * const sketch;
    };

public:
    static constexpr uint32_t cache_line_size = 512, counter_per_cache_line = 63;
    static constexpr uint32_t cache_line_size_bytes = cache_line_size / 8;

    CMSketchbookAdaptiveCountersPQ(size_t init_col_count, size_t init_row_count,
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

        init_counter_count = row_count * col_count;
        counter_count = init_counter_count;
        init_counter_count_lg = highbit_pos(counter_count) + 1;
        counter_count_lg = init_counter_count_lg;

        // Setup Prefetching
        std::fill(prefetch_queue_op, prefetch_queue_op + prefetch_queue_len, OpType::None);

        // Setup Tof Hashing
		bias_range = std::min(static_cast<uint32_t>(((64 - counter_count_lg)) / (row_count - 1)), 8U);
        bias_mask = BITMASK(bias_range);
        index_range	= std::min(static_cast<uint32_t>(64 - bias_range * row_count),
                               static_cast<uint32_t>(counter_count_lg - bias_range));
		index_mask = BITMASK(index_range);

        sketches.push_back(allocate_sketch(row_count, col_count));
        gen_seeds();

        setup_lookup_tables();
    }
    
    // Should probably write a copy constructor...
    CMSketchbookAdaptiveCountersPQ(const CMSketchbookAdaptiveCountersPQ&) = delete;
    CMSketchbookAdaptiveCountersPQ& operator=(const CMSketchbookAdaptiveCountersPQ&) = delete;

    ~CMSketchbookAdaptiveCountersPQ() {
        for (uint32_t i = 0; i < sketches.size(); i++) {
            // Delete the inner large arrays, if any
            const uint32_t cols = col_count >> (sketches.size() - i - 1);
            const uint32_t cache_line_cnt = (row_count * cols + counter_per_cache_line - 1) / counter_per_cache_line;
            for (uint32_t j = 0; j < cache_line_cnt; j++) {
                const uint64_t *words = reinterpret_cast<uint64_t *>(sketches[i] + j * cache_line_size_bytes);
                if (words[cache_line_size_words - num_extension_words] >> 63) {
                    delete reinterpret_cast<uint32_t *>(get_ptr_from_extension_bitmap(words + cache_line_size_words - 1));
                }
            }
            delete sketches[i];
        }
        sketches.clear();
        delete seeds;
    }

    template <typename T>
    void Insert(const T elem) {
        Insert(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }

    void Insert(const char *elem, const uint32_t length) {
        if (n == expansion_lim)
            expand();
        uint8_t *sketch = sketches.back();
        if constexpr (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            const uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                push_prefetch_request(sketch, pos, OpType::Insert);
                handle_last_prefetch_request(sketch);
                hash_value >>= bias_range;
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                const uint32_t pos = col_count * i + hash_key(elem, length, i);
                push_prefetch_request(sketch, pos, OpType::Insert);
                handle_last_prefetch_request(sketch);
            }
        }
        n++;
    }


    template <typename T>
    void Delete(const T elem) {
        Delete(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }


    void Delete(const char *elem, const uint32_t length) {
        uint8_t *sketch = sketches.back();
        if constexpr (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            const uint32_t old_prefetch_clock = prefetch_clock;
            prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                push_prefetch_request(sketch, pos, OpType::Delete);
                handle_last_prefetch_request(sketch);
                hash_value >>= bias_range;
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                const uint32_t pos = col_count * i + hash_key(elem, length, i);
                push_prefetch_request(sketch, pos, OpType::Delete);
                handle_last_prefetch_request(sketch);
            }
        }
        n--;
        if (n == contraction_lim)
            contract();
    }

    
    template <typename T>
    uint64_t Query(const T elem) const {
        return Query(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }


    uint64_t Query(const char *elem, const uint32_t length) const {
        uint64_t res = MAX_VALUE(8 * sizeof(uint32_t));
        const uint8_t *sketch = sketches.back();
        if constexpr (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                res = std::min(res, get_counter(sketch, pos));
                hash_value >>= bias_range;
            }
        }
        else {
            for (int i = 0; i < row_count; i++)
                res = std::min(res, get_counter(sketch, col_count * i + hash_key(elem, length, i)));
        }
        return res;
    }


    void FlushPrefetchQueue() {
        uint8_t *sketch = sketches.back();
        const uint32_t loop_clock = prefetch_clock;
        for (prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
                prefetch_clock != loop_clock;
                prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len) {
            if constexpr (using_tof_hashing) {
                switch (prefetch_queue_op[prefetch_clock]) {
                    case OpType::Insert:
                        increment_counter(sketch, prefetch_queue_cache_line[prefetch_clock],
                                                  prefetch_queue_cache_line_offset[prefetch_clock]);
                    case OpType::Delete:
                        decrement_counter(sketch, prefetch_queue_cache_line[prefetch_clock],
                                                  prefetch_queue_cache_line_offset[prefetch_clock]);
                    default:
                        ;
                }
            }
            else {
                switch (prefetch_queue_op[prefetch_clock]) {
                    case OpType::Insert:
                        increment_counter(sketch, prefetch_queue[prefetch_clock]);
                    case OpType::Delete:
                        decrement_counter(sketch, prefetch_queue[prefetch_clock]);
                    default:
                        ;
                }
            }
            prefetch_queue_op[prefetch_clock] = OpType::None;
        }
    }


    size_t Size() const {
        const uint32_t cache_line_cnt = (row_count * col_count + counter_per_cache_line - 1) / counter_per_cache_line;
        const size_t base_size = cache_line_cnt * cache_line_size_bytes;
        const uint8_t *sketch = sketches.back();
        const uint32_t cache_line_count = (row_count * col_count + counter_per_cache_line - 1) / counter_per_cache_line;
        const uint32_t sep_1 = counter_per_cache_line;
        const uint32_t sep_2 = sep_1 + counter_per_cache_line * base_counter_size;
        const uint32_t sep_3 = sep_2 + last_extension_word_bit_count;
        uint32_t extra_arrays = 0;
        for (int i = 0; i < cache_line_count; i++) {
            const uint8_t *ptr = sketch + i * cache_line_size_bytes;
            const uint64_t *words = reinterpret_cast<const uint64_t *>(ptr);
            if (words[cache_line_size_words - num_extension_words] >> 63)
                extra_arrays++;
        }
        return base_size + extra_arrays * sizeof(uint32_t) * counter_per_cache_line;
    }


private:
    uint64_t n;
    uint64_t expansion_lim, contraction_lim;
    uint32_t seed_gen_seed;
    uint32_t *seeds;
    size_t init_col_count, col_count, init_counter_count, counter_count;
    const size_t row_count;
    uint32_t init_col_count_lg, col_count_lg, init_counter_count_lg, counter_count_lg;
    std::function<uint64_t(size_t)> expansion_f;
    std::vector<uint8_t *> sketches;

    // Prefetching Queue
    static constexpr bool using_tof_hashing = true;
    static constexpr uint64_t prefetch_queue_len = 32;
    uint64_t prefetch_queue[prefetch_queue_len];
    uint64_t prefetch_queue_cache_line[prefetch_queue_len];
    uint64_t prefetch_queue_cache_line_offset[prefetch_queue_len];
    OpType prefetch_queue_op[prefetch_queue_len];
    uint32_t prefetch_clock = 0;

    // Tof Hashing Stuff
    uint32_t bias_range, index_range;
    uint64_t bias_mask, index_mask;

    static constexpr uint64_t select_mask = 0x5555555555555555;
    static constexpr uint32_t cache_line_size_words = cache_line_size / (8 * sizeof(uint64_t));
    static constexpr uint32_t base_counter_size = 6, extension_size = 2;
    static constexpr uint32_t num_extension = (cache_line_size - 1 - counter_per_cache_line * (base_counter_size + 1)) / extension_size;
    static constexpr uint32_t num_extension_words = extension_size * num_extension / 64 + 1;
    static constexpr uint64_t last_extension_word_bit_count = extension_size * num_extension + 1 - 64 * (num_extension_words - 1);
    uint8_t word_update_byte_offset[64], word_update_shamt[64];

    void setup_lookup_tables() {
        constexpr uint64_t word_size_bytes = sizeof(uint64_t);
        constexpr uint64_t word_size_bits = word_size_bytes * 8;
        uint32_t bit_pos = counter_per_cache_line;
        for (int i = 0; i < counter_per_cache_line; i++) {
            const uint32_t counter_pos = i * base_counter_size + counter_per_cache_line;
            if ((counter_pos % word_size_bits) + base_counter_size > word_size_bits) {
                word_update_byte_offset[i] = (counter_pos / word_size_bits) * word_size_bytes + word_size_bytes / 2;
                word_update_shamt[i] = counter_pos % word_size_bits - word_size_bits / 2;
            }
            else {
                word_update_byte_offset[i] = counter_pos / word_size_bits * word_size_bytes;
                word_update_shamt[i] = counter_pos % word_size_bits;
            }
        }
    }

    static_assert((8 * sizeof(uint64_t)) % extension_size == 0,
                  "Word size must be divisible by the extension size, at least for now");


    //__attribute__((always_inline))
    inline uint32_t get_cache_line_ind(const uint32_t pos) const {
        return static_cast<int32_t>(pos) / counter_per_cache_line;
    }


    __attribute__((always_inline))
    inline void push_prefetch_request(uint8_t *sketch, const uint32_t pos, const OpType op) {
        if constexpr (using_tof_hashing) {
            prefetch_queue_cache_line[prefetch_clock] = get_cache_line_ind(pos);
            prefetch_queue_cache_line_offset[prefetch_clock] = pos - counter_per_cache_line * prefetch_queue_cache_line[prefetch_clock];
            prefetch_queue_op[prefetch_clock] = op;
            __builtin_prefetch(sketch + prefetch_queue_cache_line[prefetch_clock] * cache_line_size_bytes);
        }
        else {
            prefetch_queue[prefetch_clock] = pos;
            prefetch_queue_op[prefetch_clock] = op;
            __builtin_prefetch(sketch + get_cache_line_ind(pos) * cache_line_size_bytes);
        }
        prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
    }


    __attribute__((always_inline))
    inline void handle_last_prefetch_request(uint8_t *sketch) {
        if constexpr (using_tof_hashing) {
            switch (prefetch_queue_op[prefetch_clock]) {
                case OpType::Insert:
                    increment_counter(sketch, prefetch_queue_cache_line[prefetch_clock],
                                              prefetch_queue_cache_line_offset[prefetch_clock]);
                    break;
                case OpType::Delete:
                    decrement_counter(sketch, prefetch_queue_cache_line[prefetch_clock],
                                              prefetch_queue_cache_line_offset[prefetch_clock]);
                    break;
                default:
                    break;
            }
        }
        else {
            switch (prefetch_queue_op[prefetch_clock]) {
                case OpType::Insert:
                    increment_counter(sketch, prefetch_queue[prefetch_clock]);
                    break;
                case OpType::Delete:
                    decrement_counter(sketch, prefetch_queue[prefetch_clock]);
                    break;
                default:
                    break;
            }
        }
    }


    //__attribute__((always_inline))
    inline void set_overflowing(uint64_t words[], const uint32_t pos, const bool state) {
        words[pos / 64] = state ? words[pos / 64] | (1ULL << (pos % 64))
                                : words[pos / 64] & ~(1ULL << (pos % 64));
    }

    //__attribute__((always_inline))
    inline void set_overflowing_to_0(uint64_t words[], const uint32_t pos, const uint64_t bit_value=1) {
        words[pos / 64] &= ~(bit_value << (pos % 64));
    }


    //__attribute__((always_inline))
    inline void set_overflowing_to_1(uint64_t words[], const uint32_t pos, const uint64_t bit_value=1) {
        words[pos / 64] |= bit_value << (pos % 64);
    }
    

    //__attribute__((always_inline))
    inline bool is_overflowing(const uint64_t words[], const uint32_t pos) const {
        return (words[pos / 64] >> (pos % 64)) & 1ULL;
    }


    //__attribute__((always_inline))
    inline uint64_t get_extension_mask(const uint64_t val) const {
        return (val & (val >> 1)) & select_mask;
    }


    //__attribute__((always_inline))
    inline uint32_t get_extension_rank(const uint64_t words[], const uint32_t pos) const {
        uint32_t res = 0;
        for (uint32_t i = 0; i < pos / 64; i++)
            res += __builtin_popcountll(words[i]);
        res += bit_rank(words[pos / 64], pos % 64);
        return res;
    }
    

    //__attribute__((always_inline))
    inline uint32_t get_extension_pos(const uint64_t extensions[], const uint32_t rank) const {
        uint64_t masks[num_extension_words];
        for (uint32_t i = 0; i < num_extension_words; i++)
            masks[i] = get_extension_mask(extensions[i]);
        int extension_pos = (rank == 0 ? -2 : bit_select(masks[0], rank - 1));
        if constexpr (num_extension_words == 2) {
            const int32_t other_attempt = bit_select(masks[1], rank - 1 - __builtin_popcountll(masks[0]));
            extension_pos = (extension_pos >= 64 ? other_attempt + 64 : extension_pos);
        }
        else if constexpr (num_extension_words > 2) {
            extension_pos = -3;
            uint32_t running_rank = rank - __builtin_popcountll(masks[0]);
            for (uint32_t i = 1; i < num_extension_words; i++) {
                const uint32_t select_result = running_rank > 64 ? 64 : bit_select(masks[i], running_rank - 1);
                extension_pos = ((extension_pos == -3 && select_result < 64) ? select_result + i * 64 : extension_pos);
                running_rank -= __builtin_popcountll(masks[i]);
            }
        }
        return extension_pos + 2;
    }


    //__attribute__((always_inline))
    inline uint32_t get_extension_length(uint64_t extensions[]) const {
        if constexpr (num_extension_words == 1)
            return highbit_pos(extensions[0]) + 1;
        else if constexpr (num_extension_words == 2) {
            const uint32_t a = highbit_pos(extensions[1]);
            const uint32_t b = highbit_pos(extensions[0]);
            return (a ? a + 64 : b) + 1;
        }
        else {
            uint32_t res = std::numeric_limits<uint32_t>::max();
            for (uint32_t i = num_extension_words - 1; i >= 0; i--)
                res = ((res == std::numeric_limits<uint32_t>::max() && extensions[i]) ? highbit_pos(extensions[i]) + 64 * i + 1
                                                                                      : res);
            return res;
        }
    }


    //__attribute__((always_inline))
    inline void shift_extensions_left_from_pos(uint64_t extensions[], const uint32_t pos, const uint32_t shamt) const {
        assert(shamt < 64); // Shifting more than a word not implemented
        if constexpr (num_extension_words == 2) {
            const uint64_t a = extensions[0] & BITMASK(pos);
            const uint64_t b = extensions[0] & (BITMASK(64) << pos);
            extensions[0] = (b << shamt) | a;
        }
        else if constexpr (num_extension_words == 2) {
            if (pos < 64) {
                const uint64_t a = extensions[0] & BITMASK(pos);
                const uint64_t b = extensions[0] >> pos;
                extensions[1] <<= shamt;
                extensions[1] |= (64 < pos + shamt ? b << (pos + shamt - 64) : b >> (64 - pos - shamt));
                extensions[0] &= BITMASK(pos);
                extensions[0] |= (pos + shamt >= 64 ? 0ULL : (b << (pos + shamt)));
            }
            else {
                const uint32_t new_pos = pos - 64;
                const uint64_t b = extensions[1] & (~BITMASK(new_pos));
                extensions[1] &= BITMASK(new_pos);
                extensions[1] |= b << shamt;
            }
        }
        else {
            const uint32_t pos_word = pos / 64;
            for (int32_t i = num_extension_words - 1; i > pos_word; i--) {
                const uint64_t prev_extension = i > 0 ? extensions[i - 1] & (~BITMASK(std::max(0, static_cast<int32_t>(pos) - (i - 1) * 64)))
                                                      : 0ULL;
                extensions[i] = (extensions[i] << shamt) | (prev_extension >> (64 - shamt));
            }
            const uint64_t a = extensions[pos_word] & BITMASK(pos % 64);
            const uint64_t b = extensions[pos_word] & (BITMASK(64) << (pos % 64));
            extensions[pos_word] = (b << shamt) | a;
        }
    }


    //__attribute__((always_inline))
    inline void shift_extensions_right_from_pos(uint64_t extensions[], const uint32_t pos, const uint32_t shamt) const {
        if constexpr (num_extension_words == 1) {
            const uint64_t a = extensions[0] & BITMASK(pos);
            const uint64_t b = extensions[0] & (~BITMASK(pos));
            extensions[0] = ((b >> shamt) & (~BITMASK(pos))) | a;
        }
        else if constexpr (num_extension_words == 2) {
            if (pos < 64) {
                const uint64_t a = extensions[1] & BITMASK(shamt);
                const uint64_t b = (pos + shamt >= 64 ? 0ULL : extensions[0] >> (pos + shamt));
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
        else {
            uint32_t running_prefix = pos % 64;
            for (uint32_t i = pos / 64; i < num_extension_words; i++) {
                const uint64_t a = extensions[i] & BITMASK(running_prefix);
                const uint64_t b = extensions[i] & (~BITMASK(running_prefix));
                const uint64_t c = (i < num_extension_words ? extensions[i + 1] & BITMASK(shamt) : 0UL);
                extensions[i] = (c << (64 - shamt)) | ((b >> shamt) & ~BITMASK(running_prefix)) | a;
                running_prefix = 0;
            }
        }
    }


    inline uint32_t *get_ptr_from_extension_bitmap(const uint64_t *extension_bitmap) const {
        if constexpr (num_extension_words == 1)
            return reinterpret_cast<uint32_t *>(extension_bitmap[0] & BITMASK(last_extension_word_bit_count - 1));
        return reinterpret_cast<uint32_t *>(extension_bitmap[0]);
    }


    //__attribute__((always_inline))
    inline uint64_t get_counter(const uint8_t *sketch, const uint32_t pos) const {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        const uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Calculate the base counter
        uint64_t res;
        const uint32_t base_bit_pos = inter_cache_line_ind * base_counter_size + counter_per_cache_line;
        const uint32_t base_byte_pos = base_bit_pos / 8;
        memcpy(&res, cache_line_ptr + base_byte_pos, sizeof(res));
        res = (res >> (base_bit_pos % 8)) & BITMASK(base_counter_size);

        // Take into account the extensions, if any
        const uint64_t *words = reinterpret_cast<const uint64_t *>(cache_line_ptr);
        const bool has_extension = is_overflowing(words, inter_cache_line_ind);
        if (has_extension) {
            const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
            uint64_t extension_bitmap[num_extension_words];
            for (uint32_t i = 0; i < num_extension_words - 1; i++)
                extension_bitmap[i] = words[cache_line_size_words - i - 1];
            extension_bitmap[num_extension_words - 1] = words[cache_line_size_words - num_extension_words] >> (64 - last_extension_word_bit_count);
            // Check for pointers
            if ((extension_bitmap[num_extension_words - 1] >> (last_extension_word_bit_count - 1)) & 1) {
                const uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                res |= ptr[inter_cache_line_ind] << base_counter_size;
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
                res += add_res << base_counter_size;
            }
        }

        return res;
    }


    //__attribute__((always_inline))
    inline uint32_t *setup_separate_array(const uint64_t *overflows_bitmap, uint64_t *extension_bitmap,
                                          const uint32_t total_extension_len) {
        uint32_t *ptr = new uint32_t[counter_per_cache_line];
        memset(ptr, 0, counter_per_cache_line * sizeof(uint32_t));
        uint32_t running_val = 0, running_pw = 1, cnt = 0;
        uint32_t overflows_pos = 0, running_overflows_count = 0;
        for (int i = 0; i < total_extension_len; i += extension_size) {
            const uint32_t fragment = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
            if (fragment == 3) {
                uint32_t ptr_pos = bit_select(overflows_bitmap[overflows_pos], cnt - running_overflows_count);
                while (ptr_pos == 64) {
                    running_overflows_count += __builtin_popcountll(overflows_bitmap[overflows_pos]);
                    overflows_pos++;
                    ptr_pos = bit_select(overflows_bitmap[overflows_pos], cnt - running_overflows_count);
                }
                ptr[ptr_pos] = running_val;
                running_val = 0;
                running_pw = 1;
                cnt++;
                continue;
            }
            running_val = running_val + running_pw * fragment;
            running_pw *= 3;
        }
        memset(extension_bitmap, 0, sizeof(extension_bitmap[0]) * num_extension_words);
        if constexpr (num_extension_words == 1)
            extension_bitmap[0] = reinterpret_cast<uint64_t>(ptr) & BITMASK(last_extension_word_bit_count - 1);
        else 
            extension_bitmap[0] = reinterpret_cast<uint64_t>(ptr);
        extension_bitmap[num_extension_words - 1] |= 1ULL << (last_extension_word_bit_count - 1);
        return ptr;
    }


    //__attribute__((always_inline))
    inline void write_extensions_to_cache_line(uint64_t *words, uint64_t *extension_bitmap) {
        extension_bitmap[num_extension_words - 1] = (extension_bitmap[num_extension_words - 1] << (64 - last_extension_word_bit_count))
                                                  | (words[cache_line_size_words - num_extension_words] & BITMASK(64 - last_extension_word_bit_count));
        for (uint32_t i = 0; i < num_extension_words - 1; i++)
            words[cache_line_size_words - i - 1] = extension_bitmap[i];
        words[cache_line_size_words - num_extension_words] = extension_bitmap[num_extension_words - 1];
    }


    //__attribute__((always_inline))
    inline void set_counter(uint8_t *sketch, const uint32_t pos, const uint64_t value) {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        uint64_t stamp;
        const uint32_t base_bit_pos = inter_cache_line_ind * base_counter_size + counter_per_cache_line;
        const uint32_t base_byte_pos = base_bit_pos / 8;
        memcpy(&stamp, cache_line_ptr + base_byte_pos, sizeof(stamp));
        stamp &= (~(BITMASK(base_counter_size) << (base_bit_pos % 8)));
        stamp |= (value & BITMASK(base_counter_size)) << (base_bit_pos % 8);
        memcpy(cache_line_ptr + base_byte_pos, &stamp, sizeof(stamp));
        
        // Handle the extensions
        uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
        const bool new_has_extension = value > MAX_VALUE(base_counter_size);
        const bool old_has_extension = is_overflowing(words, inter_cache_line_ind);
        const int update_state = static_cast<int>(old_has_extension) * 2 + static_cast<int>(new_has_extension);
        if (update_state) {
            const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
            uint64_t extension_bitmap[num_extension_words];
            for (uint32_t i = 0; i < num_extension_words - 1; i++)
                extension_bitmap[i] = words[cache_line_size_words - i - 1];
            extension_bitmap[num_extension_words - 1] = words[cache_line_size_words - num_extension_words] >> (64 - last_extension_word_bit_count);
            const bool already_has_array_ptr = extension_bitmap[num_extension_words - 1] >> (last_extension_word_bit_count - 1);
            const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * num_extension + 1
                                                                        : get_extension_length(extension_bitmap));
            uint32_t new_extension_len = extension_size, extension_value = value >> base_counter_size;
            for (uint64_t val = extension_value; val; val /= MAX_VALUE(extension_size))
                new_extension_len += extension_size;

            switch (update_state) {
                case 1: {   // !old_has_extension && new_has_extension
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else if (new_extension_len + total_extension_len > extension_size * num_extension) {
                        uint32_t *ptr = setup_separate_array(words, extension_bitmap, total_extension_len);
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
                        uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else if (new_extension_len + total_extension_len - old_extension_len > extension_size * num_extension) {
                        uint32_t *ptr = setup_separate_array(words, extension_bitmap, total_extension_len);
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
        set_overflowing(words, inter_cache_line_ind, new_has_extension);
    }


    //__attribute__((always_inline))
    inline void increment_counter(uint8_t *sketch, const uint32_t pos) {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        increment_counter(sketch, cache_line_ind, inter_cache_line_ind);
    }


    //__attribute__((always_inline))
    inline void increment_counter(uint8_t *sketch, const uint32_t cache_line_ind, const uint32_t inter_cache_line_ind) {
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        uint64_t *update_word = reinterpret_cast<uint64_t *>(cache_line_ptr + word_update_byte_offset[inter_cache_line_ind]);
        const uint64_t base_counter_mask = BITMASK(base_counter_size) << word_update_shamt[inter_cache_line_ind];
        const uint64_t tmp_val = (update_word[0] & base_counter_mask);
        if (tmp_val != base_counter_mask) {
            update_word[0] += 1ULL << word_update_shamt[inter_cache_line_ind];
            return;
        }

        update_word[0] &= ~base_counter_mask;
        
        // Handle the carry and extensions
        uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
        const bool has_extension = is_overflowing(words, inter_cache_line_ind);
        const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
        uint64_t extension_bitmap[num_extension_words];
        for (uint32_t i = 0; i < num_extension_words - 1; i++)
            extension_bitmap[i] = words[cache_line_size_words - i - 1];
        extension_bitmap[num_extension_words - 1] = words[cache_line_size_words - num_extension_words] >> (64 - last_extension_word_bit_count);
        const bool already_has_array_ptr = extension_bitmap[num_extension_words - 1] >> (last_extension_word_bit_count - 1);
        const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * num_extension + 1
                                                                    : get_extension_length(extension_bitmap));
        if (has_extension) {
            if (already_has_array_ptr) {
                // Update separate array
                uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
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
                        uint32_t *ptr = setup_separate_array(words, extension_bitmap, total_extension_len);
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
                uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                ptr[inter_cache_line_ind]++;
            }
            else if (total_extension_len + 2 * extension_size > extension_size * num_extension) {
                uint32_t *ptr = setup_separate_array(words, extension_bitmap, total_extension_len);
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
        set_overflowing_to_1(words, inter_cache_line_ind);
    }


    //__attribute__((always_inline))
    inline void decrement_counter(uint8_t *sketch, const uint32_t pos) {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        decrement_counter(sketch, cache_line_ind, inter_cache_line_ind);
    }


    //__attribute__((always_inline))
    inline void decrement_counter(uint8_t *sketch, const uint32_t cache_line_ind, const uint32_t inter_cache_line_ind) {
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        uint64_t *update_word = reinterpret_cast<uint64_t *>(cache_line_ptr + word_update_byte_offset[inter_cache_line_ind]);
        const uint64_t base_counter_mask = BITMASK(base_counter_size) << word_update_shamt[inter_cache_line_ind];
        const uint64_t tmp_val = (update_word[0] & base_counter_mask);
        if (tmp_val != 0) {
            update_word[0] -= 1ULL << word_update_shamt[inter_cache_line_ind];
            return;
        }

        update_word[0] |= base_counter_mask;
        
        // Handle the carry and extensions
        uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
        const bool has_extension = is_overflowing(words, inter_cache_line_ind);
        const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
        uint64_t extension_bitmap[num_extension_words];
        for (uint32_t i = 0; i < num_extension_words - 1; i++)
            extension_bitmap[i] = words[cache_line_size_words - i - 1];
        extension_bitmap[num_extension_words - 1] = words[cache_line_size_words - num_extension_words] >> (64 - last_extension_word_bit_count);
        const bool already_has_array_ptr = extension_bitmap[num_extension_words - 1] >> (last_extension_word_bit_count - 1);
        const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * num_extension + 1
                                                                    : get_extension_length(extension_bitmap));

        if (already_has_array_ptr) {
            // Update separate array
            uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
            ptr[inter_cache_line_ind]--;
            const uint64_t extensions_depleted = ptr[inter_cache_line_ind] == 0;
            set_overflowing_to_0(words, inter_cache_line_ind, extensions_depleted);
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
                set_overflowing_to_0(words, inter_cache_line_ind, extensions_depleted);
            }
        }

        write_extensions_to_cache_line(words, extension_bitmap);
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
        const uint32_t hash_shamt = counter_count_lg - init_counter_count_lg;
        uint32_t hash = (original_hash & index_mask) << bias_range;
		int32_t tmp = hash - init_counter_count;
		hash = (tmp < 0 ? hash : tmp);
        hash += ((original_hash >> (init_counter_count_lg + bias_range * row_count)) & BITMASK(hash_shamt))
                * init_counter_count;
        return hash;
    }


    inline void expand() {
        FlushPrefetchQueue();
        contraction_lim = expansion_lim;
        expansion_lim = expansion_f(2 * col_count);

        uint8_t *new_sketch = allocate_sketch(row_count, 2 * col_count);
        uint8_t *old_sketch = sketches.back();
        if constexpr (using_tof_hashing) {
            for (int i = 0; i < counter_count; i++) {
                const uint32_t old_value = get_counter(old_sketch, i);
                set_counter(new_sketch, i, old_value);
                set_counter(new_sketch, counter_count + i, old_value);
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                for (int j = 0; j < col_count; j++) {
                    const uint32_t old_value = get_counter(old_sketch, col_count * i + j);
                    set_counter(new_sketch, 2 * col_count * i + j, old_value);
                    set_counter(new_sketch, 2 * col_count * i + col_count + j, old_value);
                }
            }
        }
        sketches.push_back(new_sketch);
        col_count *= 2;
        col_count_lg++;
        counter_count *= 2;
        counter_count_lg++;
    }


    inline void contract() {
        FlushPrefetchQueue();
        expansion_lim = contraction_lim;
        contraction_lim = (n < expansion_f(2 * init_col_count) ? 0 : expansion_f(col_count / 2));

        uint8_t *new_sketch = sketches.back();
        sketches.pop_back();
        uint8_t *old_sketch = sketches.back();
        col_count /= 2;
        col_count_lg--;
        counter_count /= 2;
        counter_count_lg--;
        if constexpr (using_tof_hashing) {
            for (int i = 0; i < counter_count; i++) {
                const uint32_t a = get_counter(new_sketch, i);
                const uint32_t b = get_counter(new_sketch, counter_count + i);
                const uint32_t c = get_counter(old_sketch, i);
                set_counter(old_sketch, i, a + b - c);
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                for (int j = 0; j < col_count; j++) {
                    const uint32_t a = get_counter(new_sketch, 2 * col_count * i + j);
                    const uint32_t b = get_counter(new_sketch, 2 * col_count * i + col_count + j);
                    const uint32_t c = get_counter(old_sketch, col_count * i + j);
                    set_counter(old_sketch, col_count * i + j, a + b - c);
                }
            }
        }
        delete new_sketch;
    }
};

