#include <algorithm>
#include <limits>
#include <random>
#include <cstdint>
#include <iostream>
#include <assert.h>

#include "SublimeCMS.hpp"
#include "SublimeCS.hpp"

int main(int argc, char const *argv[]) {
    {   // Sublime_CMS
        const uint32_t init_col_count = 100;
        const uint32_t init_row_count = 3;
        auto f = [](double x) { return static_cast<uint64_t>(x * x); }; // This is the reciprocal of the size function W(N) defined in the paper.
        const uint32_t seed = 1;
        SublimeCMS sketch(init_col_count, init_row_count, f, seed);

        const std::string string_key = "abc";
        const uint64_t integer_key = 1;
        const uint64_t non_existent_key = 100;

        // Insertions
        sketch.Insert(string_key.c_str(), string_key.size());
        sketch.Insert(integer_key);
        sketch.Insert(non_existent_key);

        // Deletions
        sketch.Delete(non_existent_key);

        // Flush the recent insertions and deletions to make them visible to queries
        sketch.FlushPrefetchQueue();

        // Queries
        assert(sketch.Query(string_key.c_str(), string_key.size()) == 1);
        assert(sketch.Query(integer_key) == 1);
        assert(sketch.Query(non_existent_key) == 0);

        // Expansions, contractions, and retuning all happen under the hood!
    }

    {   // Sublime_CS
        const uint32_t init_col_count = 100;
        const uint32_t init_row_count = 3;
        auto f = [](double x) { return static_cast<uint64_t>(x * x); }; // This is the reciprocal of the size function W(F) defined in the paper.
        const uint32_t seed = 1;
        SublimeCS sketch(init_col_count, init_row_count, f, seed);

        const std::string string_key = "abc";
        const uint64_t integer_key = 1;
        const uint64_t non_existent_key = 100;

        // Insertions
        sketch.Insert(string_key.c_str(), string_key.size());
        sketch.Insert(integer_key);
        sketch.Insert(non_existent_key);

        // Deletions
        sketch.Delete(non_existent_key);

        // Flush the recent insertions and deletions to make them visible to queries
        sketch.FlushPrefetchQueue();

        // Queries
        assert(sketch.Query(string_key.c_str(), string_key.size()) == 1);
        assert(sketch.Query(integer_key) == 1);
        assert(sketch.Query(non_existent_key) == 0);

        // Expansions, contractions, and retuning all happen under the hood!
    }
}

