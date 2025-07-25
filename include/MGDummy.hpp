#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <stdexcept>
#include <sys/types.h>
#include <utility>
#include <vector>

template<typename K, typename T>
class MGDummy {
    friend class MGDummyTest;

public:
    MGDummy(size_t init_slot_count, std::function<uint64_t(size_t)> expansion_f):
                n(0),
                max_slot_count(init_slot_count),
                expansion_f(expansion_f),
                lazy_decrement(0) {
        expansion_lim = expansion_f(init_slot_count + 1);
        freq_pos.reserve(init_slot_count + 1);
        heap.reserve(init_slot_count + 1);
        heap.push_back(0);
    }

    // Should probably write a copy constructor...
    MGDummy(const MGDummy&) = delete;
    MGDummy& operator=(const MGDummy&) = delete;

    void Insert(const K elem) {
        if (n == expansion_lim)
            expand();
        auto it = freq_pos.find(elem);
        if (it != freq_pos.end()) {
            it->second.first++;
            bubble_down_heap(it->second.second);
        }
        else if (freq_pos.size() < max_slot_count) {
            freq_pos[elem] = {lazy_decrement + 1, heap.size()};
            insert_heap(elem);
        }
        else {
            lazy_decrement++;
            while (heap.size() > 1 && freq_pos[heap[1]].first <= lazy_decrement) {
                freq_pos.erase(heap[1]);
                delete_heap();
            }
        }
        n++;
    }

    void Delete(const K elem) {
        throw std::runtime_error("Deletes are not implemented");
    }

    T Query(const K elem) const {
        auto it = freq_pos.find(elem);
        if (it != freq_pos.end())
            return it->second.first - lazy_decrement;
        return 0;
    }

    size_t Size() const {
        return (sizeof(K) + sizeof(T) + sizeof(uint32_t)) / load_factor * freq_pos.size() + sizeof(K) * heap.size();
    }

private:
    static constexpr float load_factor = 0.95;
    uint64_t n;
    size_t max_slot_count;
    uint64_t expansion_lim;
    std::function<uint64_t(size_t)> expansion_f;
    T lazy_decrement;
    std::unordered_map<K, std::pair<T, uint32_t>> freq_pos;
    std::vector<K> heap;

    inline void bubble_down_heap(uint32_t pos=1) {
        const K elem = heap[pos];
        const T elem_freq = freq_pos[elem].first;
        while (pos < heap.size()) {
            T left_child_freq = 2 * pos < heap.size() ? freq_pos[heap[2 * pos]].first : std::numeric_limits<T>::max();
            T right_child_freq = 2 * pos + 1 < heap.size() ? freq_pos[heap[2 * pos + 1]].first : std::numeric_limits<T>::max();
            if (elem_freq > std::min(left_child_freq, right_child_freq)) {
                const uint32_t to_swap = 2 * pos + (left_child_freq >= right_child_freq);
                std::swap(freq_pos[elem].second, freq_pos[heap[to_swap]].second);
                std::swap(heap[pos], heap[to_swap]);
                pos = to_swap;
            }
            else 
                break;
        }
    }

    inline void bubble_up_heap(uint32_t pos=0) {
        pos = pos == 0 ? heap.size() - 1 : pos;
        for (uint32_t i = pos; i > 1; i /= 2) {
            if (freq_pos[heap[i]].first >= freq_pos[heap[i / 2]].first)
                break;
            std::swap(freq_pos[heap[i]].second, freq_pos[heap[i / 2]].second);
            std::swap(heap[i], heap[i / 2]);
        }
    }

    inline void insert_heap(const K elem) {
        heap.push_back(elem);
        bubble_up_heap();
    }

    inline void delete_heap() {
        freq_pos[heap[heap.size() - 1]].second = 1;
        heap[1] = heap[heap.size() - 1];
        heap.pop_back();
        bubble_down_heap();
    }

    inline void expand() {
        max_slot_count++;
        expansion_lim = expansion_f(max_slot_count + 1);
    }
};

