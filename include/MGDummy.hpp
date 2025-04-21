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
            if (freq_pos[heap[1]].first <= lazy_decrement) {
                freq_pos.erase(heap[1]);
                heap[1] = elem;
                freq_pos[elem] = {lazy_decrement + 1, 1};
                bubble_down_heap();
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
            return lazy_decrement < it->second.first ? it->second.first - lazy_decrement : 0;
        return 0;
    }

    size_t Size() const {
        return (sizeof(K) + sizeof(T) + sizeof(off_t)) / load_factor * freq_pos.size() + sizeof(K) * heap.size();
    }

private:
    static constexpr float load_factor = 0.95;
    uint64_t n;
    size_t max_slot_count;
    uint64_t expansion_lim;
    std::function<uint64_t(size_t)> expansion_f;
    T lazy_decrement;
    std::unordered_map<K, std::pair<T, off_t>> freq_pos;
    std::vector<K> heap;

    inline void bubble_down_heap(off_t pos=1) {
        const K elem = heap[pos];
        while (pos < heap.size()) {
            T left_child_freq = 2 * pos < heap.size() ? freq_pos[heap[2 * pos]].first : std::numeric_limits<T>::max();
            T right_child_freq = 2 * pos + 1 < heap.size() ? freq_pos[heap[2 * pos + 1]].first : std::numeric_limits<T>::max();
            if (left_child_freq < std::min(freq_pos[elem].first, right_child_freq)) {
                freq_pos[elem].second = 2 * pos;
                freq_pos[heap[2 * pos]].second = pos;
                std::swap(heap[pos], heap[2 * pos]);
                pos = 2 * pos;
            }
            else if (right_child_freq < std::min(freq_pos[elem].first, left_child_freq)) {
                freq_pos[elem].second = 2 * pos + 1;
                freq_pos[heap[2 * pos + 1]].second = pos;
                std::swap(heap[pos], heap[2 * pos + 1]);
                pos = 2 * pos + 1;
            }
            else 
                break;
        }
    }

    inline void insert_heap(const K elem) {
        heap.push_back(elem);
        for (off_t i = heap.size() - 1; i > 1; i /= 2) {
            if (freq_pos[heap[i]].first >= freq_pos[heap[i / 2]].first)
                break;
            std::swap(heap[i], heap[i / 2]);
        }
    }

    inline void expand() {
        max_slot_count++;
        expansion_lim = expansion_f(max_slot_count + 1);
    }
};

