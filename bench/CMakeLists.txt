cmake_minimum_required(VERSION 3.16.3)
project(frequency-sketch-bench)

# fetch latest argparse
include(FetchContent)
FetchContent_Declare(
    argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    GIT_TAG        v2.7
)
FetchContent_MakeAvailable(argparse)

set(CMAKE_CXX_STANDARD 17)

# The harness (bench_utils.hpp, bench_template.hpp) takes its byte swap and bit
# scans from include/portability.hpp, so every target here needs include/ --
# workload_gen and the baselines too, whose libraries do not export it. (Not
# PROJECT_SOURCE_DIR: this directory declares a project of its own.)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include)

list(APPEND Targets "SublimeCMS" "SublimeCMSNoTuning" "SublimeCMSNoTuningMorris"
                    "CMS"
                    "StingyCM"
                    "CodingCM"
                    "SALSACM"
                    "SublimeCS" "SublimeCSl2" "SublimeCSNoTuning"
                    "CS"
                    "StingyC"
                    "CodingC"
                    "Tailored"
                    "OTailored"
                    "Waving"
                    "SublimeMG" "SublimeMGNoTuning"
                    "MG" "MGHeap"
                    "SpaceSaving")

# SALSA is the one baseline tied to x86: it seeds its RNG from `__rdtsc` and
# uses AVX2. It is a pinned third-party submodule and stays untouched, so it is
# left out of the build where it cannot compile -- off x86, and on x86 machines
# without AVX2 -- `run_benchmarks.py` skips a bench that was not built, and the
# figures are drawn without it. Every other baseline builds on x86-64 and ARM64
# Linux and macOS alike, checked by full cross-builds. (Stingy, Coding and
# Tailored Sketch used to include <mmintrin.h>, which exists only on x86, without
# using anything from it; the include is gone and their x86 code is unchanged.)
if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86)$")
    message(STATUS "Not an x86 machine (${CMAKE_SYSTEM_PROCESSOR}): skipping the SALSA baseline, which is x86-only")
    list(REMOVE_ITEM Targets "SALSACM")
else()
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "${NATIVE_FLAG}")
    check_cxx_source_compiles("
        #include <immintrin.h>
        int main() { __m256i v = _mm256_set_epi32(1, 2, 3, 4, 5, 6, 7, 8); return _mm256_extract_epi32(v, 0); }"
        SUBLIME_BENCH_HAS_AVX2)
    unset(CMAKE_REQUIRED_FLAGS)
    if (NOT SUBLIME_BENCH_HAS_AVX2)
        message(STATUS "No AVX2 on this machine: skipping the SALSA baseline, which needs it")
        list(REMOVE_ITEM Targets "SALSACM")
    endif()
endif()

function(compile_bench ds)
    if (NOT ds IN_LIST Targets)
        message(WARNING "${ds} is not defined")
        return()
    endif()

    if (ds STREQUAL "SublimeMGNoTuning")
        # One glue file serves both Sublime_MG builds: this one just defines the
        # macro that makes it include the generated fixed-tuning header first.
        add_executable(bench_${ds} sketches_benchmark/bench_SublimeMG.cpp)
        target_compile_definitions(bench_${ds} PRIVATE SUBLIME_MG_FIXED_TUNING)
    else()
        add_executable(bench_${ds} sketches_benchmark/bench_${ds}.cpp)
    endif()
    set_target_properties(bench_${ds} PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(bench_${ds} argparse)

    if (ds STREQUAL "CMS")
        target_link_libraries(bench_${ds} CMSLib)
    elseif (ds STREQUAL "SublimeCMS")
        target_link_libraries(bench_${ds} SublimeCMSLib)
    elseif (ds STREQUAL "SublimeCMSNoTuning")
        target_link_libraries(bench_${ds} SublimeCMSNoTuningLib)
    elseif (ds STREQUAL "SublimeCMSNoTuningMorris")
        target_link_libraries(bench_${ds} SublimeCMSNoTuningMorrisLib)
    elseif (ds STREQUAL "StingyCM")
        add_library(StingyCMLib STATIC ./sketches_benchmark/include/StingyCM.hpp 
                                       ./sketches_benchmark/include/MurmurHash.h 
                                       ./sketches_benchmark/include/params/stingy.h)
        set_target_properties(StingyCMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(StingyCMLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} StingyCMLib)
    elseif (ds STREQUAL "CodingCM")
        add_library(CodingCMLib STATIC ./sketches_benchmark/include/CodingCM.hpp 
                                       ./sketches_benchmark/include/MurmurHash.h 
                                       ./sketches_benchmark/include/params/coding.h)
        set_target_properties(CodingCMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(CodingCMLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} CodingCMLib)
    elseif (ds STREQUAL "SALSACM")
        add_library(SALSACMLib STATIC ./sketches_benchmark/include/salsa/Salsa/SalsaCMS.hpp
                                      ./sketches_benchmark/include/salsa/Salsa/SalsaCMS.cpp
                                      ./sketches_benchmark/include/salsa/Salsa/BobHash.hpp
                                      ./sketches_benchmark/include/salsa/Salsa/BobHash.cpp)
        target_include_directories(SALSACMLib PUBLIC ./sketches_benchmark/include/salsa/Salsa/)
        set_target_properties(SALSACMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(SALSACMLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} SALSACMLib)
    elseif (ds STREQUAL "CS")
        target_link_libraries(bench_${ds} CSLib)
    elseif (ds STREQUAL "SublimeCS")
        target_link_libraries(bench_${ds} SublimeCSLib)
    elseif (ds STREQUAL "SublimeCSl2")
        target_link_libraries(bench_${ds} SublimeCSLib)
    elseif (ds STREQUAL "SublimeCSNoTuning")
        target_link_libraries(bench_${ds} SublimeCSNoTuningLib)
    elseif (ds STREQUAL "StingyC")
        add_library(StingyCLib STATIC ./sketches_benchmark/include/StingyC.hpp 
                                      ./sketches_benchmark/include/MurmurHash.h 
                                      ./sketches_benchmark/include/params/stingy.h)
        set_target_properties(StingyCLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(StingyCLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} StingyCLib)
    elseif (ds STREQUAL "CodingC")
        add_library(CodingCLib STATIC ./sketches_benchmark/include/CodingC.hpp
                                      ./sketches_benchmark/include/MurmurHash.h 
                                      ./sketches_benchmark/include/params/coding.h)
        set_target_properties(CodingCLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(CodingCLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} CodingCLib)
    elseif (ds MATCHES ".*Tailored")
        add_library(${ds}Lib STATIC ./sketches_benchmark/include/${ds}.hpp)
        set_target_properties(${ds}Lib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(${ds}Lib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} ${ds}Lib)
    elseif (ds STREQUAL "Waving")
        add_library(WavingLib STATIC ./sketches_benchmark/include/waving_sketch/include/Waving.h ./sketches_benchmark/include/waving_sketch/include/bitset.h)
        target_include_directories(WavingLib PUBLIC ./sketches_benchmark/include/waving_sketch/include/)
        set_target_properties(WavingLib PROPERTIES LINKER_LANGUAGE CXX)
        # WavingSketch uses `usleep` without including <unistd.h>; newer glibc
        # and musl do not pull it in transitively. Forced here rather than
        # patched into the submodule, which kept it dirty in `git status`.
        target_compile_options(WavingLib PUBLIC ${BITHACKING_COMPILE_FLAGS} -include unistd.h)
        # WavingSketch calls `std::random_shuffle`, which C++17 removed. GCC's
        # standard library still has it; LLVM's libc++ -- macOS's, and Clang's
        # elsewhere -- brings it back only when asked. No effect under libstdc++.
        target_compile_definitions(WavingLib PUBLIC _LIBCPP_ENABLE_CXX17_REMOVED_RANDOM_SHUFFLE)
        target_link_libraries(bench_${ds} WavingLib)
    elseif (ds STREQUAL "SublimeMG")
        target_link_libraries(bench_${ds} SublimeMGLib)
    elseif (ds STREQUAL "SublimeMGNoTuning")
        target_link_libraries(bench_${ds} SublimeMGNoTuningLib)
    elseif (ds STREQUAL "MG")
        target_link_libraries(bench_${ds} MGLib)
    elseif (ds STREQUAL "MGHeap")
        target_link_libraries(bench_${ds} MGHeapLib)
    elseif (ds STREQUAL "SpaceSaving")
        target_link_libraries(bench_${ds} SpaceSavingLib)
    endif()
endfunction(compile_bench)

foreach(ds ${Targets})
    compile_bench(${ds})
endforeach()

# Setup workload generation binary
add_executable(workload_gen workload_gen.cpp zipf/zipf.h zipf/zipf.c)
target_link_libraries(workload_gen argparse)

