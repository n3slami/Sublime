#pragma once

#include <algorithm>
#include <bits/floatn-common.h>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <vector>

#include "MurmurHash.hpp"
#include "util.hpp"

class SublimeCS {
    friend class SublimeCSTest;

public:
    // Tuning Stuff
    static constexpr uint32_t cache_line_size = 512;
    static constexpr uint32_t cache_line_size_bytes = cache_line_size / 8;
    static constexpr uint32_t cache_line_size_words = cache_line_size_bytes / sizeof(uint64_t);
    static constexpr uint32_t min_counter_per_cache_line = 16, max_counter_per_cache_line = 92, default_counter_per_cache_line = 68;
    static_assert(min_counter_per_cache_line <= default_counter_per_cache_line 
               && default_counter_per_cache_line <= max_counter_per_cache_line);

private:
    static constexpr uint64_t select_mask = 0x5555555555555555;
    static constexpr uint32_t min_stub_size = 4, max_stub_size = 32, default_stub_size = 5;
    static_assert(min_stub_size <= default_stub_size 
               && default_stub_size <= max_stub_size);
    static constexpr uint32_t extension_size = 2;
    static constexpr uint32_t min_extension_count = 24, max_extension_count = 64;
    static constexpr float max_spill_probability = 0.01, retune_spill_frac = 0.03;
    static constexpr auto bit_length_to_extension_count = setup_extension_len_lookup_table();

    struct Sketch {
        uint32_t counter_count;
        uint32_t col_count;
        uint32_t row_count;
        uint32_t cache_line_count;
        uint32_t spilled_cache_lines;
        uint32_t spill_retune_limit;
        uint32_t counter_per_cache_line;
        uint32_t stub_size;
        uint64_t stub_mask;
        uint32_t num_extension;
        uint32_t num_extension_words;
        uint32_t last_extension_word_bit_count;
        uint8_t word_update_byte_offset[max_counter_per_cache_line], word_update_shamt[max_counter_per_cache_line];
        const uint8_t padding[20];
        uint8_t sketch[0];
    };

    static_assert(sizeof(Sketch) % cache_line_size_bytes == 0);

    // Operation Type for Prefetching Queue
    enum class OpType {
        Insert,
        Delete,
        Query,
        None
    };

public:
    SublimeCS(size_t init_col_count, size_t init_row_count, std::function<uint64_t(double)> expansion_f, uint32_t seed_gen_seed)
                                     : row_count(init_row_count), 
                                       init_col_count(init_col_count),
                                       expansion_f(expansion_f),
                                       seed_gen_seed(seed_gen_seed) {
        n = 0;
        col_count = init_col_count;
        expansion_lim = expansion_f(col_count);
        contraction_lim = expansion_f(col_count / 2.0);
        init_col_count_lg = highbit_pos(init_col_count) + (__builtin_popcountll(init_col_count) > 1);
        col_count_lg = init_col_count_lg;

        init_counter_count = row_count * col_count;
        counter_count = init_counter_count;
        init_counter_count_lg = highbit_pos(counter_count) + (__builtin_popcountll(counter_count) > 1);
        counter_count_lg = init_counter_count_lg;

        // Setup Prefetching
        std::fill(prefetch_queue_op, prefetch_queue_op + prefetch_queue_len, OpType::None);

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
    SublimeCS(const SublimeCS&) = delete;
    SublimeCS& operator=(const SublimeCS&) = delete;

    ~SublimeCS() {
        while (!sketches.empty()) {
            Sketch *sketch = sketches.back();
            free_spills(sketch);
            delete[] sketch;
            sketches.pop_back();
        }
        delete[] seeds;
    }

    template <typename T>
    void Insert(const T elem) {
        Insert(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }

    void Insert(const char *elem, const uint32_t length) {
        if (n == expansion_lim)
            expand();
        Sketch *sketch = sketches.back();
        if constexpr (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            const uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                hash_value >>= bias_range;
                push_prefetch_request(sketch, pos, hash_value & 1ULL, OpType::Insert);
                handle_last_prefetch_request(sketch);
                hash_value >>= 1;
            }
        }
        else {
            uint64_t sign_hash = get_sign_hash(elem, length);
            for (int i = 0; i < row_count; i++) {
                const uint32_t pos = col_count * i + hash_key(elem, length, i);
                push_prefetch_request(sketch, pos, sign_hash & 1ULL, OpType::Insert);
                handle_last_prefetch_request(sketch);
                sign_hash >>= 1;
            }
        }
        n++;

        if (sketch->spilled_cache_lines >= sketch->spill_retune_limit) {
            FlushPrefetchQueue();
            reallocate_sketch(sketch);
            sketches[sketches.size() - 1] = sketch;
        }
    }


    template <typename T>
    void Delete(const T elem) {
        Delete(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }

    void Delete(const char *elem, const uint32_t length) {
        Sketch *sketch = sketches.back();
        if constexpr (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            const uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                hash_value >>= bias_range;
                push_prefetch_request(sketch, pos, hash_value & 1ULL, OpType::Delete);
                handle_last_prefetch_request(sketch);
                hash_value >>= 1;
            }
        }
        else {
            uint64_t sign_hash = get_sign_hash(elem, length);
            for (int i = 0; i < row_count; i++) {
                const uint32_t pos = col_count * i + hash_key(elem, length, i);
                push_prefetch_request(sketch, pos, sign_hash & 1ULL, OpType::Delete);
                handle_last_prefetch_request(sketch);
                sign_hash >>= 1;
            }
        }
        n--;
        if (n < contraction_lim)
            contract();
        if (n == (contraction_lim + expansion_lim) / 2) {
            FlushPrefetchQueue();
            reallocate_sketch(sketch, false);
            sketches[sketches.size() - 1] = sketch;
        }
    }


    template <typename T>
    int64_t Query(const T elem) const {
        return Query(reinterpret_cast<const char *>(&elem), sizeof(elem));
    }

    int64_t Query(const char *elem, const uint32_t length) const {
        int64_t res[row_count];
        const Sketch *sketch = sketches.back();
        if constexpr (using_tof_hashing) {
            uint64_t hash_value = MurmurHash64B(elem, length, seeds[0]);
            const uint32_t index = hash_tof(hash_value);
            hash_value >>= index_range;
            for (int i = 0; i < row_count; i++) {
                uint32_t pos = index + (i << bias_range) + (hash_value & bias_mask);
                pos = pos < counter_count ? pos : pos - counter_count;
                const int64_t raw_val = get_counter(sketch, pos);
                hash_value >>= bias_range;
                res[i] = (hash_value & 1ULL) ? raw_val : -raw_val;
                hash_value >>= 1;
            }
        }
        else {
            uint64_t sign_hash = get_sign_hash(elem, length);
            for (int i = 0; i < row_count; i++) {
                const int64_t raw_val = get_counter(sketch, col_count * i + hash_key(elem, length, i));
                res[i] = (sign_hash & 1ULL) ? raw_val : -raw_val;
                sign_hash >>= 1;
            }
        }
        std::sort(res, res + row_count);
        return res[(row_count - 1) / 2];
    }


    void FlushPrefetchQueue() {
        Sketch *sketch = sketches.back();
        const uint32_t loop_clock = prefetch_clock;
        prefetch_queue_op[loop_clock] = OpType::None;
        for (prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
                prefetch_clock != loop_clock;
                prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len) {
            handle_last_prefetch_request(sketch);
            prefetch_queue_op[prefetch_clock] = OpType::None;
        }
    }


    size_t Size(bool include_all_sketches=false) const {
        size_t res = 0;
        for (int32_t sketch_ind = sketches.size() - 1; sketch_ind >= 0; sketch_ind--) {
            const Sketch *sketch = sketches[sketch_ind];
            const uint32_t cache_line_count = (sketch->row_count * sketch->col_count + sketch->counter_per_cache_line - 1) 
                / sketch->counter_per_cache_line;
            const size_t base_size = cache_line_count * cache_line_size_bytes;
            const uint32_t sep_1 = sketch->counter_per_cache_line;
            const uint32_t sep_2 = sep_1 + sketch->counter_per_cache_line * sketch->stub_size;
            const uint32_t sep_3 = sep_2 + sketch->last_extension_word_bit_count;
            uint32_t extra_arrays = 0;
            for (int i = 0; i < cache_line_count; i++) {
                const uint8_t *ptr = sketch->sketch + i * cache_line_size_bytes;
                const uint64_t *words = reinterpret_cast<const uint64_t *>(ptr);
                if (has_separate_array(sketch, words))
                    extra_arrays++;
            }
            res += base_size + extra_arrays * sizeof(uint32_t) * sketch->counter_per_cache_line;
            if (!include_all_sketches)
                break;
        }
        return res;
    }


    uint32_t GetCountersPerChunk() const {
        return sketches.back()->counter_per_cache_line;
    }


    uint32_t GetStubLength() const {
        return sketches.back()->stub_size;
    }


private:
    uint64_t n;
    uint64_t expansion_lim, contraction_lim;
    uint32_t seed_gen_seed, sign_seed;
    uint32_t *seeds;
    size_t init_col_count, col_count, init_counter_count, counter_count;
    const size_t row_count;
    uint32_t init_col_count_lg, col_count_lg, init_counter_count_lg, counter_count_lg;
    std::function<uint64_t(double)> expansion_f;
    std::vector<Sketch *> sketches;

    // Prefetching Queue
    static constexpr uint64_t prefetch_queue_len = 32;
    uint64_t prefetch_queue[prefetch_queue_len];
    uint64_t prefetch_queue_cache_line[prefetch_queue_len];
    uint64_t prefetch_queue_cache_line_offset[prefetch_queue_len];
    bool prefetch_queue_sign[prefetch_queue_len];
    OpType prefetch_queue_op[prefetch_queue_len];
    uint32_t prefetch_clock = 0;

    // Tof Hashing Stuff
    static constexpr bool using_tof_hashing = true;
    uint32_t bias_range, index_range;
    uint64_t bias_mask, index_mask;

    inline void setup_lookup_tables(Sketch *sketch) {
        constexpr uint64_t word_size_bytes = sizeof(uint64_t);
        constexpr uint64_t word_size_bits = word_size_bytes * 8;
        for (int i = 0; i < sketch->counter_per_cache_line; i++) {
            const uint32_t counter_pos = i * sketch->stub_size + sketch->counter_per_cache_line;
            if ((counter_pos % word_size_bits) + sketch->stub_size > word_size_bits) {
                sketch->word_update_byte_offset[i] = (counter_pos / word_size_bits) * word_size_bytes + word_size_bytes / 2;
                sketch->word_update_shamt[i] = counter_pos % word_size_bits - word_size_bits / 2;
            }
            else {
                sketch->word_update_byte_offset[i] = counter_pos / word_size_bits * word_size_bytes;
                sketch->word_update_shamt[i] = counter_pos % word_size_bits;
            }
        }
    }

    static_assert((8 * sizeof(uint64_t)) % extension_size == 0,
                  "Word size must be divisible by the extension size, at least for now");

    
    //__attribute__((always_inline))
    inline uint32_t get_cache_line_ind(const Sketch *sketch, const uint32_t pos) const {
        return static_cast<int32_t>(pos) / sketch->counter_per_cache_line;
    }


    __attribute__((always_inline))
    inline void push_prefetch_request(Sketch *sketch, const uint32_t pos, const bool sign, const OpType op) {
        if constexpr (using_tof_hashing) {
            prefetch_queue_cache_line[prefetch_clock] = get_cache_line_ind(sketch, pos);
            prefetch_queue_cache_line_offset[prefetch_clock] = pos - sketch->counter_per_cache_line * prefetch_queue_cache_line[prefetch_clock];
            __builtin_prefetch(sketch + prefetch_queue_cache_line[prefetch_clock] * cache_line_size_bytes);
        }
        else {
            prefetch_queue[prefetch_clock] = pos;
            __builtin_prefetch(sketch + get_cache_line_ind(sketch, pos) * cache_line_size_bytes);
        }
        prefetch_queue_sign[prefetch_clock] = sign;
        prefetch_queue_op[prefetch_clock] = op;
        prefetch_clock = (prefetch_clock + 1) % prefetch_queue_len;
    }


    __attribute__((always_inline))
    inline void handle_last_prefetch_request(Sketch *sketch) {
        if constexpr (using_tof_hashing) {
            switch (prefetch_queue_op[prefetch_clock]) {
                case OpType::Insert:
                    incdec_counter(sketch, prefetch_queue_cache_line[prefetch_clock],
                                           prefetch_queue_cache_line_offset[prefetch_clock],
                                           prefetch_queue_sign[prefetch_clock]);
                    break;
                case OpType::Delete:
                    incdec_counter(sketch, prefetch_queue_cache_line[prefetch_clock],
                                           prefetch_queue_cache_line_offset[prefetch_clock],
                                           1 ^ prefetch_queue_sign[prefetch_clock]);
                    break;
                default:
                    break;
            }
        }
        else {
            switch (prefetch_queue_op[prefetch_clock]) {
                case OpType::Insert:
                    incdec_counter(sketch, prefetch_queue[prefetch_clock],
                                           prefetch_queue_sign[prefetch_clock]);
                    break;
                case OpType::Delete:
                    incdec_counter(sketch, prefetch_queue[prefetch_clock],
                                           1 ^ prefetch_queue_sign[prefetch_clock]);
                    break;
                default:
                    break;
            }
        }
    }

    __attribute__((always_inline))
    inline void set_overflowing(uint64_t words[], const uint32_t pos, const bool state) {
        words[pos / 64] = state ? words[pos / 64] | (1ULL << (pos % 64))
                                : words[pos / 64] & ~(1ULL << (pos % 64));
    }

    __attribute__((always_inline))
    inline void set_overflowing_to_0(uint64_t words[], const uint32_t pos, const uint64_t bit_value=1) {
        words[pos / 64] &= ~(bit_value << (pos % 64));
    }


    __attribute__((always_inline))
    inline void set_overflowing_to_1(uint64_t words[], const uint32_t pos, const uint64_t bit_value=1) {
        words[pos / 64] |= bit_value << (pos % 64);
    }
    

    __attribute__((always_inline))
    inline bool is_overflowing(const uint64_t words[], const uint32_t pos) const {
        return (words[pos / 64] >> (pos % 64)) & 1ULL;
    }


    __attribute__((always_inline))
    inline uint64_t get_extension_mask(const uint64_t val) const {
        return (val & (val >> 1)) & select_mask;
    }


    __attribute__((always_inline))
    inline uint32_t get_extension_rank(const uint64_t words[], const uint32_t pos) const {
        uint32_t res = 0;
        for (uint32_t i = 0; i < pos / 64; i++)
            res += __builtin_popcountll(words[i]);
        res += bit_rank(words[pos / 64], pos % 64);
        return res;
    }


    __attribute__((always_inline))
    inline uint32_t get_extension_pos(const Sketch *sketch, const uint64_t extensions[], const uint32_t rank) const {
        uint64_t masks[sketch->num_extension_words];
        for (uint32_t i = 0; i < sketch->num_extension_words; i++)
            masks[i] = get_extension_mask(extensions[i]);
        int extension_pos = (rank == 0 ? -2 : bit_select(masks[0], rank - 1));
        if (sketch->num_extension_words == 2) {
            const int32_t other_attempt = bit_select(masks[1], rank - 1 - __builtin_popcountll(masks[0]));
            extension_pos = (extension_pos >= 64 ? other_attempt + 64 : extension_pos);
        }
        else if (sketch->num_extension_words > 2) {
            extension_pos = (extension_pos < 64 ? extension_pos : -3);
            uint32_t running_rank = rank - __builtin_popcountll(masks[0]);
            for (uint32_t i = 1; i < sketch->num_extension_words; i++) {
                const uint32_t select_result = running_rank > 64 ? 64 : bit_select(masks[i], running_rank - 1);
                extension_pos = ((extension_pos == -3 && select_result < 64) ? select_result + i * 64 : extension_pos);
                running_rank -= __builtin_popcountll(masks[i]);
            }
        }
        return extension_pos + 2;
    }


    __attribute__((always_inline))
    inline uint32_t get_extension_length(const Sketch *sketch, uint64_t extensions[]) const {
        if (sketch->num_extension_words == 1)
            return highbit_pos(extensions[0]) + 1;
        else if (sketch->num_extension_words == 2) {
            const uint32_t a = highbit_pos(extensions[1]);
            const uint32_t b = highbit_pos(extensions[0]);
            return (a ? a + 64 : b) + 1;
        }
        else {
            uint32_t res = 0;
            for (int32_t i = sketch->num_extension_words - 1; i >= 0; i--)
                res = std::max(res, (extensions[i] ? highbit_pos(extensions[i]) + 64 * i + 1 : 0));
            return res;
        }
    }


    //__attribute__((always_inline))
    inline void shift_extensions_left_from_pos(Sketch *sketch, uint64_t extensions[], const uint32_t pos, const uint32_t shamt) const {
        assert(shamt < 64); // Shifting more than a word not implemented
        if (sketch->num_extension_words == 1) {
            const uint64_t a = extensions[0] & BITMASK(pos);
            const uint64_t b = extensions[0] & (BITMASK(64) << pos);
            extensions[0] = (b << shamt) | a;
        }
        else if (sketch->num_extension_words == 2) {
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
            const int32_t pos_word = pos / 64;
            for (int32_t i = sketch->num_extension_words - 1; i > pos_word; i--) {
                const uint64_t prev_extension = i > pos_word ? extensions[i - 1] & (~BITMASK(std::max(0, static_cast<int32_t>(pos) - (i - 1) * 64)))
                                                             : 0ULL;
                extensions[i] = (extensions[i] << shamt) | (prev_extension >> (64 - shamt));
            }
            const uint64_t a = extensions[pos_word] & BITMASK(pos % 64);
            const uint64_t b = extensions[pos_word] & (BITMASK(64) << (pos % 64));
            extensions[pos_word] = (b << shamt) | a;
        }
    }


    //__attribute__((always_inline))
    inline void shift_extensions_right_from_pos(Sketch *sketch, uint64_t extensions[], const uint32_t pos, const uint32_t shamt) const {
        if (sketch->num_extension_words == 1) {
            const uint64_t a = extensions[0] & BITMASK(pos);
            const uint64_t b = extensions[0] & (~BITMASK(pos));
            extensions[0] = ((b >> shamt) & (~BITMASK(pos))) | a;
        }
        else if (sketch->num_extension_words == 2) {
            if (pos < 64) {
                const bool end_in_second_word = pos + shamt >= 64;
                const uint32_t dead_a = end_in_second_word ? pos + shamt - 64 : 0;
                const uint64_t a = (extensions[1] >> dead_a) & BITMASK(shamt - dead_a);
                const uint64_t b = (end_in_second_word ? 0ULL : extensions[0] >> (pos + shamt));
                extensions[1] >>= shamt;
                extensions[0] &= BITMASK(pos);
                extensions[0] |= ((a << (64 - shamt + dead_a)) | (b << pos));
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
            for (uint32_t i = pos / 64; i < sketch->num_extension_words; i++) {
                const uint64_t a = extensions[i] & BITMASK(running_prefix);
                const uint64_t b = extensions[i] & (~BITMASK(running_prefix));
                const uint64_t c = (i < sketch->num_extension_words ? extensions[i + 1] & BITMASK(shamt) : 0UL);
                extensions[i] = (c << (64 - shamt)) | ((b >> shamt) & ~BITMASK(running_prefix)) | a;
                running_prefix = 0;
            }
        }
    }


    inline uint32_t *get_ptr_from_extension_bitmap(const uint64_t *extension_bitmap) const {
        return reinterpret_cast<uint32_t *>(extension_bitmap[0] & BITMASK(min_extension_count * extension_size));
    }


    //__attribute__((always_inline))
    inline int64_t get_counter(const Sketch *sketch, const uint32_t pos) const {
        const uint32_t cache_line_ind = get_cache_line_ind(sketch, pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * sketch->counter_per_cache_line;
        const uint8_t *cache_line_ptr = sketch->sketch + cache_line_ind * cache_line_size_bytes;

        // Calculate the stub
        const uint64_t *read_word = reinterpret_cast<const uint64_t *>(cache_line_ptr + sketch->word_update_byte_offset[inter_cache_line_ind]);
        int64_t res = (read_word[0] >> sketch->word_update_shamt[inter_cache_line_ind]) & (sketch->stub_mask >> 1);
        const int64_t sign = (read_word[0] >> (sketch->word_update_shamt[inter_cache_line_ind] + sketch->stub_size - 1)) & 1LL;

        // Take into account the extensions, if any
        const uint64_t *words = reinterpret_cast<const uint64_t *>(cache_line_ptr);
        const bool has_extension = is_overflowing(words, inter_cache_line_ind);
        if (has_extension) {
            const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
            uint64_t extension_bitmap[sketch->num_extension_words];
            for (uint32_t i = 0; i < sketch->num_extension_words - 1; i++)
                extension_bitmap[i] = words[cache_line_size_words - i - 1];
            extension_bitmap[sketch->num_extension_words - 1] = words[cache_line_size_words - sketch->num_extension_words] 
                                                                >> (64 - sketch->last_extension_word_bit_count);
            // Check for pointers
            if ((extension_bitmap[sketch->num_extension_words - 1] >> (sketch->last_extension_word_bit_count - 1)) & 1) {
                const uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                res |= ptr[inter_cache_line_ind] << (sketch->stub_size - 1);
            }
            else {
                uint64_t add_res = 0, add_pw = 1;
                for (int i = get_extension_pos(sketch, extension_bitmap, extension_rank); true; i += extension_size) {
                    const uint32_t new_bits = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
                    if (new_bits == MAX_VALUE(extension_size))
                        break;
                    add_res += new_bits * add_pw;
                    add_pw *= MAX_VALUE(extension_size);
                }
                res += add_res << (sketch->stub_size - 1);
            }
        }

        return sign ? -res : res;
    }

    //__attribute__((always_inline))
    inline uint32_t *setup_separate_array(Sketch *sketch, const uint64_t *overflows_bitmap,
                                          uint64_t *extension_bitmap, const uint32_t total_extension_len) {
        uint32_t *ptr = new uint32_t[sketch->counter_per_cache_line];
        memset(ptr, 0, sketch->counter_per_cache_line * sizeof(uint32_t));
        uint32_t running_val = 0, running_pw = 1, cnt = 0;
        uint32_t overflows_pos = 0, running_overflows_count = 0;
        for (int i = 0; i < total_extension_len; i += extension_size) {
            const uint32_t fragment = (extension_bitmap[i / 64] >> (i % 64)) & BITMASK(extension_size);
            if (fragment == MAX_VALUE(extension_size)) {
                uint32_t ptr_pos = bit_select(overflows_bitmap[overflows_pos], cnt - running_overflows_count);
                while (ptr_pos == 64) {
                    running_overflows_count += __builtin_popcountll(overflows_bitmap[overflows_pos]);
                    overflows_pos++;
                    ptr_pos = bit_select(overflows_bitmap[overflows_pos], cnt - running_overflows_count);
                }
                ptr_pos += 64 * overflows_pos;
                ptr[ptr_pos] = running_val;
                running_val = 0;
                running_pw = 1;
                cnt++;
                continue;
            }
            running_val = running_val + running_pw * fragment;
            running_pw *= 3;
        }
        memset(extension_bitmap, 0, sizeof(extension_bitmap[0]) * sketch->num_extension_words);
        if (sketch->num_extension_words == 1)
            extension_bitmap[0] = reinterpret_cast<uint64_t>(ptr) & BITMASK(sketch->last_extension_word_bit_count - 1);
        else 
            extension_bitmap[0] = reinterpret_cast<uint64_t>(ptr);
        extension_bitmap[sketch->num_extension_words - 1] |= 1ULL << (sketch->last_extension_word_bit_count - 1);
        sketch->spilled_cache_lines++;
        return ptr;
    }


    __attribute__((always_inline))
    inline bool has_separate_array(const Sketch *sketch, const uint64_t *cache_line_words) const {
        return cache_line_words[cache_line_size_words - sketch->num_extension_words] >> 63;
    }

    
    //__attribute__((always_inline))
    inline void write_extensions_to_cache_line(Sketch *sketch, uint64_t *words, uint64_t *extension_bitmap) {
        extension_bitmap[sketch->num_extension_words - 1] = (extension_bitmap[sketch->num_extension_words - 1] << (64 - sketch->last_extension_word_bit_count))
                                          | (words[cache_line_size_words - sketch->num_extension_words] & BITMASK(64 - sketch->last_extension_word_bit_count));
        for (uint32_t i = 0; i < sketch->num_extension_words - 1; i++)
            words[cache_line_size_words - i - 1] = extension_bitmap[i];
        words[cache_line_size_words - sketch->num_extension_words] = extension_bitmap[sketch->num_extension_words - 1];
    }


    //__attribute__((always_inline))
    inline void set_counter(Sketch *sketch, const uint32_t pos, const int64_t _value) {
        const int64_t sign = _value < 0;
        const int64_t value = _value < 0 ? -_value : _value;

        const uint32_t cache_line_ind = get_cache_line_ind(sketch, pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * sketch->counter_per_cache_line;
        uint8_t *cache_line_ptr = sketch->sketch + cache_line_ind * cache_line_size_bytes;

        // Set the stub
        uint64_t *write_word = reinterpret_cast<uint64_t *>(cache_line_ptr + sketch->word_update_byte_offset[inter_cache_line_ind]);
        write_word[0] &= ~(sketch->stub_mask << sketch->word_update_shamt[inter_cache_line_ind]);
        write_word[0] |= ((sign << (sketch->stub_size - 1)) | (value & (sketch->stub_mask >> 1))) 
                            << sketch->word_update_shamt[inter_cache_line_ind];

        // Handle the extensions
        uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
        const bool new_has_extension = value > MAX_VALUE(sketch->stub_size - 1);
        const bool old_has_extension = is_overflowing(words, inter_cache_line_ind);
        const int update_state = static_cast<int>(old_has_extension) * 2 + static_cast<int>(new_has_extension);
        if (update_state) {
            const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
            uint64_t extension_bitmap[sketch->num_extension_words];
            for (uint32_t i = 0; i < sketch->num_extension_words - 1; i++)
                extension_bitmap[i] = words[cache_line_size_words - i - 1];
            extension_bitmap[sketch->num_extension_words - 1] = words[cache_line_size_words - sketch->num_extension_words] 
                                                                >> (64 - sketch->last_extension_word_bit_count);
            const bool already_has_array_ptr = has_separate_array(sketch, words);
            const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * sketch->num_extension + 1
                                                                        : get_extension_length(sketch, extension_bitmap));
            uint32_t new_extension_len = extension_size, extension_value = value >> (sketch->stub_size - 1);
            for (uint64_t val = extension_value; val; val /= MAX_VALUE(extension_size))
                new_extension_len += extension_size;

            switch (update_state) {
                case 1: {   // !old_has_extension && new_has_extension
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else if (new_extension_len + total_extension_len > extension_size * sketch->num_extension) {
                        uint32_t *ptr = setup_separate_array(sketch, words, extension_bitmap, total_extension_len);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else {
                        // Handle Locally
                        const uint32_t pos = get_extension_pos(sketch, extension_bitmap, extension_rank);
                        shift_extensions_left_from_pos(sketch, extension_bitmap, pos, new_extension_len);
                        uint64_t tmp_val = extension_value, i;
                        for (i = pos; tmp_val; tmp_val /= MAX_VALUE(extension_size), i += extension_size)
                            extension_bitmap[i / 64] |= ((tmp_val % MAX_VALUE(extension_size)) << (i % 64));
                        extension_bitmap[i / 64] |= (MAX_VALUE(extension_size) << (i % 64));
                    }
                    break;
                }
                case 2: {   // old_has_extension && !new_has_extension
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                        ptr[inter_cache_line_ind] = 0;
                    }
                    else {
                        // Handle Locally
                        const uint32_t pos = get_extension_pos(sketch, extension_bitmap, extension_rank);
                        const uint32_t old_extension_len = get_extension_pos(sketch, extension_bitmap, extension_rank + 1) - pos;
                        shift_extensions_right_from_pos(sketch, extension_bitmap, pos, old_extension_len);
                    }
                    break;
                }
                case 3: {   // old_has_extension && new_has_extension
                    const uint32_t pos = get_extension_pos(sketch, extension_bitmap, extension_rank);
                    const uint32_t old_extension_len = get_extension_pos(sketch, extension_bitmap, extension_rank + 1) - pos;
                    if (already_has_array_ptr) {
                        // Update separate array
                        uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else if (new_extension_len + total_extension_len - old_extension_len > extension_size * sketch->num_extension) {
                        uint32_t *ptr = setup_separate_array(sketch, words, extension_bitmap, total_extension_len);
                        ptr[inter_cache_line_ind] = extension_value;
                    }
                    else {
                        // Handle Locally
                        shift_extensions_right_from_pos(sketch, extension_bitmap, pos, old_extension_len);
                        shift_extensions_left_from_pos(sketch, extension_bitmap, pos, new_extension_len);
                        uint64_t tmp_val = extension_value, i;
                        for (i = pos; tmp_val; tmp_val /= MAX_VALUE(extension_size), i += extension_size)
                            extension_bitmap[i / 64] |= ((tmp_val % MAX_VALUE(extension_size)) << (i % 64));
                        extension_bitmap[i / 64] |= (MAX_VALUE(extension_size) << (i % 64));
                    }
                    break;
                }
            }

            write_extensions_to_cache_line(sketch, words, extension_bitmap);
        }
        set_overflowing(words, inter_cache_line_ind, new_has_extension);
    }

    inline void incdec_counter(Sketch *sketch, const uint32_t pos, const int32_t inc) {
        const uint32_t cache_line_ind = get_cache_line_ind(sketch, pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * sketch->counter_per_cache_line;
        incdec_counter(sketch, cache_line_ind, inter_cache_line_ind, inc);
    }

    inline void incdec_counter(Sketch *sketch, const uint32_t cache_line_ind, const uint32_t inter_cache_line_ind,
                               const int32_t inc) {
        uint8_t *cache_line_ptr = sketch->sketch + cache_line_ind * cache_line_size_bytes;

        // Set the stub
        uint64_t *update_word = reinterpret_cast<uint64_t *>(cache_line_ptr + sketch->word_update_byte_offset[inter_cache_line_ind]);
        const uint64_t stub_mask = (sketch->stub_mask >> 1) << sketch->word_update_shamt[inter_cache_line_ind];
        const int64_t val = (update_word[0] & stub_mask);
        const int64_t val_sign = (update_word[0] >> (sketch->word_update_shamt[inter_cache_line_ind] + sketch->stub_size - 1))
                                    & 1LL;
        const int64_t op = val_sign ^ inc;
        if (op) {
            if (val != stub_mask) {
                update_word[0] += 1ULL << sketch->word_update_shamt[inter_cache_line_ind];
                return;
            }

            update_word[0] &= ~stub_mask;

            // Handle the carry and extensions
            uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
            const bool has_extension = is_overflowing(words, inter_cache_line_ind);
            const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
            uint64_t extension_bitmap[sketch->num_extension_words];
            for (uint32_t i = 0; i < sketch->num_extension_words - 1; i++)
                extension_bitmap[i] = words[cache_line_size_words - i - 1];
            extension_bitmap[sketch->num_extension_words - 1] = words[cache_line_size_words - sketch->num_extension_words] 
                                                                >> (64 - sketch->last_extension_word_bit_count);
            const bool already_has_array_ptr = has_separate_array(sketch, words);
            const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * sketch->num_extension + 1
                                                                        : get_extension_length(sketch, extension_bitmap));
            if (has_extension) {
                if (already_has_array_ptr) {
                    // Update separate array
                    uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                    ptr[inter_cache_line_ind]++;
                }
                else {
                    const uint32_t pos = get_extension_pos(sketch, extension_bitmap, extension_rank);
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
                        if (total_extension_len + extension_size > extension_size * sketch->num_extension) {
                            uint32_t *ptr = setup_separate_array(sketch, words, extension_bitmap, total_extension_len);
                            ptr[inter_cache_line_ind] = 1;
                            for (int j = pos; j < i; j += 2)
                                ptr[inter_cache_line_ind] *= 3;
                        }
                        else {
                            shift_extensions_left_from_pos(sketch, extension_bitmap, i, extension_size);
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
                else if (total_extension_len + 2 * extension_size > extension_size * sketch->num_extension) {
                    uint32_t *ptr = setup_separate_array(sketch, words, extension_bitmap, total_extension_len);
                    ptr[inter_cache_line_ind] = 1;
                }
                else {
                    const uint32_t pos = get_extension_pos(sketch, extension_bitmap, extension_rank);
                    shift_extensions_left_from_pos(sketch, extension_bitmap, pos, 2 * extension_size);
                    extension_bitmap[pos / 64] += 1ULL << (pos % 64);
                    extension_bitmap[(pos + 2) / 64] |= MAX_VALUE(extension_size) << ((pos + 2) % 64);
                }
            }

            set_overflowing_to_1(words, inter_cache_line_ind);
            write_extensions_to_cache_line(sketch, words, extension_bitmap);
        }
        else {
            if (val != 0) {
                update_word[0] -= 1ULL << sketch->word_update_shamt[inter_cache_line_ind];
                return;
            }

            uint64_t *words = reinterpret_cast<uint64_t *>(cache_line_ptr);
            const bool has_extension = is_overflowing(words, inter_cache_line_ind);
            if (!has_extension) {
                update_word[0] ^= ((1LL << (sketch->stub_size - 1)) | 1LL) << sketch->word_update_shamt[inter_cache_line_ind];
                return;
            }
            update_word[0] |= stub_mask;

            // Handle the carry and extensions
            const uint32_t extension_rank = get_extension_rank(words, inter_cache_line_ind);
            uint64_t extension_bitmap[sketch->num_extension_words];
            for (uint32_t i = 0; i < sketch->num_extension_words - 1; i++)
                extension_bitmap[i] = words[cache_line_size_words - i - 1];
            extension_bitmap[sketch->num_extension_words - 1] = words[cache_line_size_words - sketch->num_extension_words] 
                                                                >> (64 - sketch->last_extension_word_bit_count);
            const bool already_has_array_ptr = has_separate_array(sketch, words);
            const uint32_t total_extension_len = (already_has_array_ptr ? extension_size * sketch->num_extension + 1
                                                                        : get_extension_length(sketch, extension_bitmap));

            if (already_has_array_ptr) {
                // Update separate array
                uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                ptr[inter_cache_line_ind]--;
                const uint64_t extensions_depleted = ptr[inter_cache_line_ind] == 0;
                set_overflowing_to_0(words, inter_cache_line_ind, extensions_depleted);
            }
            else {
                const uint32_t pos = get_extension_pos(sketch, extension_bitmap, extension_rank);
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
                    shift_extensions_right_from_pos(sketch, extension_bitmap, shift_pos, shamt);
                    set_overflowing_to_0(words, inter_cache_line_ind, extensions_depleted);
                }
            }

            write_extensions_to_cache_line(sketch, words, extension_bitmap);
        }
    }


    inline void increment_counter(Sketch *sketch, const uint32_t pos) {
        incdec_counter(sketch, pos, 1);
    }

    inline void decrement_counter(Sketch *sketch, const uint32_t pos) {
        incdec_counter(sketch, pos, 0);
    }


    inline std::pair<uint32_t, uint32_t> tune_params(const uint32_t *counter_len_cnt) {
        if (counter_len_cnt == nullptr)
            return {default_counter_per_cache_line, default_stub_size};
        const uint32_t word_size_bits = 8 * sizeof(uint64_t);
        uint64_t counter_len_ps[word_size_bits] = {counter_len_cnt[0]};
        for (uint32_t i = 1; i < word_size_bits; i++)
            counter_len_ps[i] = counter_len_ps[i - 1] + counter_len_cnt[i];
        for (int32_t m_c = max_counter_per_cache_line; m_c >= min_counter_per_cache_line; m_c--) {
            const uint32_t cache_line_cnt = counter_len_ps[word_size_bits - 1] / m_c;
            for (uint32_t m_s = min_stub_size; m_s <= max_stub_size; m_s++) {
                const int32_t extension_bitmap_len = cache_line_size - m_c * (m_s + 1) - 1;
                if (extension_bitmap_len > static_cast<int32_t>(extension_size * max_extension_count))
                    continue;
                if (extension_bitmap_len < static_cast<int32_t>(extension_size * min_extension_count))
                    break;
                float expected_extension_len = 0, var_extension_len = 0;
                for (uint32_t i = m_s + 1; i < word_size_bits; i++) {
                    const uint64_t term = extension_size * bit_length_to_extension_count[i - m_s];
                    const float expected_term = static_cast<float>(term) / cache_line_cnt;
                    const float var_term = static_cast<float>(term * term) / cache_line_cnt - expected_term * expected_term;
                    expected_extension_len += expected_term * counter_len_cnt[i];
                    var_extension_len += var_term * counter_len_cnt[i];
                }
                const float deviation = extension_bitmap_len - expected_extension_len;
                if (deviation < 0)
                    continue;
                if (var_extension_len / (deviation * deviation) > max_spill_probability) // Chebyshev's Inequality. Perhaps we can use Chernoff too?
                    continue;
                return {m_c, m_s};
            }
        }
        return {std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()};
    }

    inline Sketch *allocate_sketch(const uint32_t rows, const uint32_t cols, const uint32_t *counter_len_cnt=nullptr) {
        auto [counter_per_cache_line, stub_size] = tune_params(counter_len_cnt);
        const uint32_t cache_line_count = 50 + (rows * cols + counter_per_cache_line - 1) / counter_per_cache_line;
        Sketch *res = reinterpret_cast<Sketch *>(new uint8_t[sizeof(Sketch) + cache_line_count * cache_line_size_bytes]);
        memset(res + 1, 0, cache_line_count * cache_line_size_bytes);
        res->counter_count = rows * cols;
        res->col_count = cols;
        res->row_count = rows;
        res->cache_line_count = cache_line_count;
        res->spilled_cache_lines = 0;
        res->spill_retune_limit = cache_line_count * retune_spill_frac + 1;
        res->counter_per_cache_line = counter_per_cache_line;
        res->stub_size = stub_size;
        res->stub_mask = BITMASK(stub_size);
        res->num_extension = (cache_line_size - 1 - counter_per_cache_line * (stub_size + 1)) / extension_size;
        res->num_extension_words = extension_size * res->num_extension / 64 + 1;
        res->last_extension_word_bit_count = extension_size * res->num_extension + 1 - 64 * (res->num_extension_words - 1);
        setup_lookup_tables(res);
        return res;
    }


    inline void reallocate_sketch(Sketch *&sketch, bool decreasing=true) {
        uint32_t counter_len_cnt[8 * sizeof(uint64_t)] = {};
        compute_counter_len_cnt(sketch, counter_len_cnt);
        auto [counter_per_cache_line, stub_size] = tune_params(counter_len_cnt);

        if (decreasing && stub_size == sketch->stub_size)
            counter_per_cache_line = std::min(counter_per_cache_line, sketch->counter_per_cache_line - 1);

        const uint32_t cache_line_count = 50 + (sketch->counter_count + counter_per_cache_line - 1) / counter_per_cache_line;
        Sketch *res = reinterpret_cast<Sketch *>(new uint8_t[sizeof(Sketch) + cache_line_count * cache_line_size_bytes]);
        memset(res + 1, 0, cache_line_count * cache_line_size_bytes);
        res->counter_count = sketch->counter_count;
        res->col_count = sketch->col_count;
        res->row_count = sketch->row_count;
        res->cache_line_count = cache_line_count;
        res->spilled_cache_lines = 0;
        res->spill_retune_limit = cache_line_count * retune_spill_frac + 1;
        res->counter_per_cache_line = counter_per_cache_line;
        res->stub_size = stub_size;
        res->stub_mask = BITMASK(stub_size);
        res->num_extension = (cache_line_size - 1 - counter_per_cache_line * (stub_size + 1)) / extension_size;
        res->num_extension_words = extension_size * res->num_extension / 64 + 1;
        res->last_extension_word_bit_count = extension_size * res->num_extension + 1 - 64 * (res->num_extension_words - 1);
        setup_lookup_tables(res);

        for (uint32_t i = 0; i < sketch->counter_count; i++)
            set_counter(res, i, get_counter(sketch, i));
        free_spills(sketch);
        delete[] sketch;
        sketch = res;
    }


    inline void gen_seeds() {
        seeds = new uint32_t[row_count];
        std::mt19937 rng(seed_gen_seed);
        for (int i = 0; i < row_count; i++)
            seeds[i] = rng();
        sign_seed = rng();
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


    inline uint64_t get_sign_hash(const char *key, const uint32_t length) const {
        return MurmurHash64B(key, length, sign_seed);
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


    inline void compute_counter_len_cnt(const Sketch *sketch, uint32_t *counter_len_cnt) {
        for (uint32_t i = 0; i < sketch->counter_count; i++)
            counter_len_cnt[highbit_pos(static_cast<uint64_t>(std::abs(get_counter(sketch, i)))) + 1]++;
    }


    inline void free_spills(Sketch *sketch) {
        for (uint32_t chunk = 0; chunk < sketch->cache_line_count; chunk++) {
            uint64_t *words = reinterpret_cast<uint64_t *>(sketch->sketch + cache_line_size_bytes * chunk);
            uint64_t extension_bitmap[sketch->num_extension_words];
            for (uint32_t i = 0; i < sketch->num_extension_words - 1; i++)
                extension_bitmap[i] = words[cache_line_size_words - i - 1];
            extension_bitmap[sketch->num_extension_words - 1] = words[cache_line_size_words - sketch->num_extension_words] 
                                                                >> (64 - sketch->last_extension_word_bit_count);
            if (has_separate_array(sketch, words)) {
                uint32_t *ptr = get_ptr_from_extension_bitmap(extension_bitmap);
                delete[] ptr;
            }
        }
    }


    inline void expand() {
        FlushPrefetchQueue();
        contraction_lim = expansion_lim;
        expansion_lim = expansion_f(2 * col_count);

        Sketch  *old_sketch = sketches.back();
        uint32_t counter_len_cnt[8 * sizeof(uint64_t)] = {};
        compute_counter_len_cnt(old_sketch, counter_len_cnt);
        Sketch *new_sketch = allocate_sketch(row_count, 2 * col_count);
        if constexpr (using_tof_hashing) {
            for (int i = 0; i < counter_count; i++) {
                const int64_t old_value = get_counter(old_sketch, i);
                set_counter(new_sketch, i, old_value);
                set_counter(new_sketch, counter_count + i, old_value);
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                for (int j = 0; j < col_count; j++) {
                    const int64_t old_value = get_counter(old_sketch, col_count * i + j);
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
        contraction_lim = (n <= expansion_f(init_col_count) ? 0 : expansion_f(col_count / 4.0));

        Sketch *new_sketch = sketches.back();
        sketches.pop_back();
        Sketch *old_sketch = sketches.back();
        col_count /= 2;
        col_count_lg--;
        counter_count /= 2;
        counter_count_lg--;
        if constexpr (using_tof_hashing) {
            for (int i = 0; i < counter_count; i++) {
                const int64_t a = get_counter(new_sketch, i);
                const int64_t b = get_counter(new_sketch, counter_count + i);
                const int64_t c = get_counter(old_sketch, i);
                set_counter(old_sketch, i, a + b - c);
            }
        }
        else {
            for (int i = 0; i < row_count; i++) {
                for (int j = 0; j < col_count; j++) {
                    const int64_t a = get_counter(new_sketch, 2 * col_count * i + j);
                    const int64_t b = get_counter(new_sketch, 2 * col_count * i + col_count + j);
                    const int64_t c = get_counter(old_sketch, col_count * i + j);
                    set_counter(old_sketch, col_count * i + j, a + b - c);
                }
            }
        }
        free_spills(new_sketch);
        delete[] new_sketch;
    }
};

