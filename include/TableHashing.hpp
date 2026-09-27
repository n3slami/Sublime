#pragma once

/*
 * ============================================================================
 *
 *        TableHashing
 *          The hash functions a fingerprint table hashes its keys with, and
 *          the one bit-twiddling helper that goes with them.
 *
 *        Taken verbatim from Memento filter, by way of the rank-and-select
 *        quotient filter this repository used to carry, so that the hashing
 *        semantics of the structures agree.
 *
 * ============================================================================
 */

#include <cstdint>
#include <cstring>

#include "util.hpp"

namespace sublime {

/**
 * MurmurHash2, 64-bit version, by Austin Appleby.
 */
inline uint64_t MurmurHash64A(const void *key, int32_t len, uint32_t seed) {
    const uint64_t m = 0xc6a4a7935bd1e995;
    const int r = 47;

    uint64_t h = seed ^ (len * m);

    const uint64_t *data = (const uint64_t *) key;
    const uint64_t *end = data + (len / 8);

    while (data != end) {
        uint64_t k = *data++;

        k *= m;
        k ^= k >> r;
        k *= m;

        h ^= k;
        h *= m;
    }

    const unsigned char *data2 = (const unsigned char *) data;

    switch (len & 7) {
        case 7: h ^= (uint64_t) data2[6] << 48;  /* fallthrough */
        case 6: h ^= (uint64_t) data2[5] << 40;  /* fallthrough */
        case 5: h ^= (uint64_t) data2[4] << 32;  /* fallthrough */
        case 4: h ^= (uint64_t) data2[3] << 24;  /* fallthrough */
        case 3: h ^= (uint64_t) data2[2] << 16;  /* fallthrough */
        case 2: h ^= (uint64_t) data2[1] << 8;   /* fallthrough */
        case 1: h ^= (uint64_t) data2[0];
                h *= m;
    };

    h ^= h >> r;
    h *= m;
    h ^= h >> r;

    return h;
}

/** Thomas Wang's integer hash function. */

inline uint64_t hash_64(uint64_t key, uint64_t mask) {
    key = (~key + (key << 21)) & mask;
    key = key ^ key >> 24;
    key = ((key + (key << 3)) + (key << 8)) & mask;
    key = key ^ key >> 14;
    key = ((key + (key << 2)) + (key << 4)) & mask;
    key = key ^ key >> 28;
    key = (key + (key << 31)) & mask;
    return key;
}

namespace fpt {

/** `BITMASK` from `util.hpp` as a function, to keep macros out of this file. */
__attribute__((always_inline))
static constexpr inline uint64_t bitmask(uint32_t nbits) {
    return nbits >= 64 ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << nbits) - 1);
}

}   // namespace fpt

}   // namespace sublime
