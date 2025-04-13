#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

#include "MurmurHash.hpp"
#include "util.hpp"

class CMSketchbookAdaptiveCounters {
    friend class CMSketchbookAdaptiveCountersTest;

public:
    CMSketchbookAdaptiveCounters(size_t init_col_count, size_t init_row_count,
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

    ~CMSketchbookAdaptiveCounters() {
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
        sketches.clear();
        delete seeds;
    }

    void Insert(const uint64_t elem) {
        if (n == expansion_lim)
            expand();
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++)
            increment_counter(sketch, col_count * i + hash_key(elem, i));
        n++;
    }

    void Insert(const char *elem, const uint32_t length) {
        if (n == expansion_lim)
            expand();
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++)
            increment_counter(sketch, col_count * i + hash_string_key(elem, length, i));
        n++;
    }

    void Delete(const uint64_t elem) {
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++)
            decrement_counter(sketch, col_count * i + hash_key(elem, i));
        n--;
        if (n == contraction_lim)
            contract();
    }

    void Delete(const char *elem, const uint32_t length) {
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++)
            decrement_counter(sketch, col_count * i + hash_string_key(elem, length, i));
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

    uint64_t Query(const char *elem, const uint32_t length) {
        uint64_t res = MAX_VALUE(8 * sizeof(uint32_t));
        uint8_t *sketch = sketches.back();
        for (int i = 0; i < row_count; i++)
            res = std::min(res, get_counter(sketch, col_count * i + hash_string_key(elem, length, i)));
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

    static const uint64_t select_mask = 0x5555555555555555;
    static const uint32_t cache_line_size = 512, counter_per_cache_line = 57;
    static const uint32_t cache_line_size_bytes = cache_line_size / 8;
    static const uint32_t cache_line_size_words = cache_line_size / (8 * sizeof(uint64_t));
    static const uint32_t base_counter_size = 6, extension_size = 2;
    static const uint32_t num_extension = (cache_line_size - 1 - counter_per_cache_line * (base_counter_size + 1)) / extension_size;
    static const uint64_t second_word_bit_count = cache_line_size - counter_per_cache_line * (base_counter_size + 1) - 64;

    static_assert((8 * sizeof(uint64_t)) % extension_size == 0,
                  "Word size must be divisible by the extension size, at least for now");

    //__attribute__((always_inline))
    inline uint32_t get_cache_line_ind(const uint32_t pos) {
        return static_cast<int32_t>(pos) / counter_per_cache_line;
    }

    //__attribute__((always_inline))
    inline uint64_t get_extension_mask(const uint64_t val) {
        return (val & (val >> 1)) & select_mask;
    }

    //__attribute__((always_inline))
    inline uint32_t get_extension_pos(const uint64_t extensions[], const uint32_t rank) {
        const uint64_t masks[2] = {get_extension_mask(extensions[0]), get_extension_mask(extensions[1])};
        int32_t extension_pos = (rank == 0 ? -2 : bit_select(masks[0], rank - 1));
        const int32_t other_attempt = bit_select(masks[1], rank - 1 - __builtin_popcountll(masks[0]));
        extension_pos = (extension_pos >= 64 ? other_attempt + 64 : extension_pos);
        return extension_pos + 2;
    }

    //__attribute__((always_inline))
    inline uint32_t get_extension_length(uint64_t extensions[]) {
        const uint32_t a = highbit_pos(extensions[1]);
        const uint32_t b = highbit_pos(extensions[0]);
        return (a ? a + 64 : b) + 1;
    }

    //__attribute__((always_inline))
    inline void shift_extensions_left_from_pos(uint64_t extensions[], const uint32_t pos, const uint32_t shamt) {
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
    inline void shift_extensions_right_from_pos(uint64_t extensions[], const uint32_t pos, const uint32_t shamt) {
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
    inline uint64_t get_counter(const uint8_t *sketch, const uint32_t pos) {
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
        const bool has_extension = (words[0] >> inter_cache_line_ind) & 1;
        if (has_extension) {
            const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
            uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                            words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
            // Check for pointers
            if ((extension_bitmap[1] >> (second_word_bit_count - 1)) & 1) {
                const uint32_t *ptr = reinterpret_cast<const uint32_t *>(extension_bitmap[0]);
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
        const bool old_has_extension = (words[0] >> inter_cache_line_ind) & 1;
        const int update_state = static_cast<int>(old_has_extension) * 2 + static_cast<int>(new_has_extension);
        if (update_state) {
            const uint32_t extension_rank = bit_rank(words[0], inter_cache_line_ind);
            uint64_t extension_bitmap[2] = {words[cache_line_size_words - 1],
                                            words[cache_line_size_words - 2] >> (64 - second_word_bit_count)};
            const bool already_has_array_ptr = extension_bitmap[1] >> (second_word_bit_count - 1);
            const uint32_t total_extension_len = (already_has_array_ptr ? 2 * num_extension + 1
                                                                        : get_extension_length(extension_bitmap));
            uint32_t new_extension_len = extension_size, extension_value = value >> base_counter_size;
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

    //__attribute__((always_inline))
    inline void increment_counter(uint8_t *sketch, const uint32_t pos) {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        uint64_t stamp;
        const uint32_t base_bit_pos = inter_cache_line_ind * base_counter_size + counter_per_cache_line;
        const uint32_t base_byte_pos = base_bit_pos / 8;
        memcpy(&stamp, cache_line_ptr + base_byte_pos, sizeof(stamp));
        uint64_t val = (stamp >> (base_bit_pos % 8)) & BITMASK(base_counter_size);
        const uint64_t carried = (val == MAX_VALUE(base_counter_size));
        stamp += (carried ^ 1ULL) << (base_bit_pos % 8);
        stamp &= ~((BITMASK(base_counter_size) << (base_bit_pos % 8)) & (-carried));
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

    //__attribute__((always_inline))
    inline void decrement_counter(uint8_t *sketch, const uint32_t pos) {
        const uint32_t cache_line_ind = get_cache_line_ind(pos);
        const uint32_t inter_cache_line_ind = pos - cache_line_ind * counter_per_cache_line;
        uint8_t *cache_line_ptr = sketch + cache_line_ind * cache_line_size_bytes;

        // Set the base counter
        uint64_t stamp;
        const uint32_t base_bit_pos = inter_cache_line_ind * base_counter_size + counter_per_cache_line;
        const uint32_t base_byte_pos = base_bit_pos / 8;
        memcpy(&stamp, cache_line_ptr + base_byte_pos, sizeof(stamp));
        uint64_t val = (stamp >> (base_bit_pos % 8)) & BITMASK(base_counter_size);
        const uint64_t carried = (val == 0);
        stamp -= (carried ^ 1ULL) << (base_bit_pos % 8);
        stamp |= ((BITMASK(base_counter_size) << (base_bit_pos % 8)) & (-carried));
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

    inline uint8_t *allocate_sketch(const size_t rows, const size_t cols) {
        const uint32_t cache_line_cnt = (rows * cols + counter_per_cache_line - 1) / counter_per_cache_line;
        const uint32_t size = cache_line_cnt * cache_line_size_bytes;
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

