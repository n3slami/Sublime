#pragma once

/*
 * The compiler- and ISA-specific spellings Sublime uses, each with a fallback,
 * so the sketches build with GCC and Clang on x86-64 and AArch64 (Linux,
 * macOS) and with MSVC on Windows.
 *
 * Three layers, picked at compile time:
 *
 *  - x86 instruction-set extensions (BMI, BMI2, LZCNT, AVX-512F) are used only
 *    when the target has them, which is what `-march=native` decides. Each has
 *    a scalar path, so `SUBLIME_HAS_*` is never required, only faster.
 *  - Compiler builtins (`__builtin_popcountll`, `__builtin_ctzll`,
 *    `__builtin_prefetch`, ...) are used under GCC and Clang on any
 *    architecture; MSVC gets its own intrinsics; anything else gets plain C++.
 *  - `<immintrin.h>` is included on x86 only. It used to be included
 *    unconditionally, which is an `#error` on every ARM machine.
 *
 * Two macros force the fallbacks on a machine that would not otherwise take
 * them, which is the only way to test them on the x86 Linux box the results
 * are produced on:
 *
 *  - `SUBLIME_PORTABLE` disables every x86-specific path, as on an ARM target;
 *  - `SUBLIME_NO_BUILTINS` additionally replaces the GCC/Clang builtins and
 *    variable-length arrays with the plain C++ a compiler with neither would
 *    use (and MSVC uses: fixed-bound arrays, and on ARM64 the popcount).
 */

#include <cstdint>

#if !defined(SUBLIME_PORTABLE) && !defined(SUBLIME_NO_BUILTINS) \
        && (defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86))
#define SUBLIME_X86 1
#include <immintrin.h>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define SUBLIME_MSVC 1
#include <intrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && !defined(SUBLIME_NO_BUILTINS)
#define SUBLIME_GNU_BUILTINS 1
#endif

// x86 extensions, only where both the target and the compiler offer them.
#if defined(SUBLIME_X86) && defined(SUBLIME_GNU_BUILTINS) && defined(__BMI__)
#define SUBLIME_HAS_BMI 1
#endif
#if defined(SUBLIME_X86) && defined(__BMI2__)
#define SUBLIME_HAS_BMI2 1
#endif
#if defined(SUBLIME_X86) && defined(SUBLIME_GNU_BUILTINS) && defined(__LZCNT__)
#define SUBLIME_HAS_LZCNT 1
#endif
#if defined(SUBLIME_X86) && defined(__AVX512F__)
#define SUBLIME_HAS_AVX512F 1
#endif


/**
 * A stack array of `size` elements, which never exceeds `bound`. GCC and Clang
 * take it as a variable-length array, exactly as the code was always written --
 * fixing the size there changes the generated code of the counter routines
 * (`SublimeCMS::increment_counter` grew by 30%), and the x86 Linux binaries the
 * results come from are meant to stay instruction for instruction what they
 * were. Elsewhere VLAs are an error, so it is `bound` elements instead.
 */
#if defined(SUBLIME_GNU_BUILTINS)
#define SUBLIME_STACK_ARRAY(type, name, size, bound) type name[size]
#else
#define SUBLIME_STACK_ARRAY(type, name, size, bound) type name[bound]
#endif


/** Inline, overriding the compiler's own judgement. */
#if defined(SUBLIME_MSVC)
#define SUBLIME_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define SUBLIME_ALWAYS_INLINE __attribute__((always_inline))
#else
#define SUBLIME_ALWAYS_INLINE
#endif


/** A read prefetch of the cache line holding `addr`; a no-op where there is none. */
#if defined(SUBLIME_GNU_BUILTINS)
#define SUBLIME_PREFETCH(addr) __builtin_prefetch(addr)
#elif defined(SUBLIME_MSVC) && (defined(_M_X64) || defined(_M_IX86))
#define SUBLIME_PREFETCH(addr) _mm_prefetch(reinterpret_cast<const char *>(addr), _MM_HINT_T0)
#elif defined(SUBLIME_MSVC) && defined(_M_ARM64)
#define SUBLIME_PREFETCH(addr) __prefetch(addr)
#else
#define SUBLIME_PREFETCH(addr) ((void) (addr))
#endif


/** `x` with its bytes in reverse order. */
SUBLIME_ALWAYS_INLINE
static inline uint64_t byteswap64(uint64_t x) {
#if defined(SUBLIME_GNU_BUILTINS)
    return __builtin_bswap64(x);
#elif defined(SUBLIME_MSVC)
    return _byteswap_uint64(x);
#else
    x = ((x & 0x00FF00FF00FF00FFULL) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFULL);
    x = ((x & 0x0000FFFF0000FFFFULL) << 16) | ((x >> 16) & 0x0000FFFF0000FFFFULL);
    return (x << 32) | (x >> 32);
#endif
}


/** The number of set bits in `x`. */
SUBLIME_ALWAYS_INLINE
static inline uint32_t popcount64(uint64_t x) {
#if defined(SUBLIME_GNU_BUILTINS)
    return static_cast<uint32_t>(__builtin_popcountll(x));
#elif defined(SUBLIME_MSVC) && defined(_M_X64) && defined(__AVX__)
    // `popcnt` is not baseline x64, but every CPU with AVX has it.
    return static_cast<uint32_t>(__popcnt64(x));
#else
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<uint32_t>((x * 0x0101010101010101ULL) >> 56);
#endif
}


/** The index of the lowest set bit of `x`, which must not be zero. */
SUBLIME_ALWAYS_INLINE
static inline uint32_t ctz64_nonzero(uint64_t x) {
#if defined(SUBLIME_GNU_BUILTINS)
    return static_cast<uint32_t>(__builtin_ctzll(x));
#elif defined(SUBLIME_MSVC) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long index;
    _BitScanForward64(&index, x);
    return static_cast<uint32_t>(index);
#else
    return popcount64((x & (0 - x)) - 1);
#endif
}


/** The index of the highest set bit of `x`, which must not be zero. */
SUBLIME_ALWAYS_INLINE
static inline uint32_t msb64_nonzero(uint64_t x) {
#if defined(SUBLIME_GNU_BUILTINS)
    return 63 - static_cast<uint32_t>(__builtin_clzll(x));
#elif defined(SUBLIME_MSVC) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long index;
    _BitScanReverse64(&index, x);
    return static_cast<uint32_t>(index);
#else
    uint32_t res = 0;
    for (uint32_t step = 32; step > 0; step >>= 1) {
        if (x >> step) {
            x >>= step;
            res += step;
        }
    }
    return res;
#endif
}
