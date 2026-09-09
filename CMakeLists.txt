cmake_minimum_required(VERSION 3.16.3)
project(Sublime
        VERSION 1.0
        HOMEPAGE_URL https://github.com/)

option(BUILD_TESTS "Build the tests" ON)
option(BUILD_EXAMPLES "Build the examples" ON)
option(BUILD_BENCHMARKS "Build the benchmark targets" ON)

set(FIXED_TUNING_C "64" CACHE STRING "The number of counters per chunk used in the fixed tuning version of Sublime")
set(FIXED_TUNING_S "6" CACHE STRING "The stub length used in the fixed tuning version of Sublime")
message(STATUS "Sublime fixed tuning VALE parameters: c=${FIXED_TUNING_C} s=${FIXED_TUNING_S}")
set(LOG_REC_INC_PROB "3" CACHE STRING "The base-2 logarithm of the reciprocal of the increment probability used in the version of Sublime that uses Morris counters")

set(CMAKE_CXX_STANDARD 17)
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
else()
    set(USE_MULTI_THREADED OFF)
endif ()

set(BITHACKING_COMPILE_FLAGS -Ofast -march=native)
#set(BITHACKING_COMPILE_FLAGS -Ofast -mbmi2 -msse4.2 -D__SSE4_2_)

add_library(CMSLib STATIC ./include/CMS.hpp)
set_target_properties(CMSLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSLib PUBLIC ./include)
target_compile_options(CMSLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(SublimeCMSLib STATIC ./include/SublimeCMS.hpp)
set_target_properties(SublimeCMSLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(SublimeCMSLib PUBLIC ./include)
target_compile_options(SublimeCMSLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

configure_file(./include/SublimeCMSNoTuning.hpp.in ./include/SublimeCMSNoTuning.hpp @ONLY)
add_library(SublimeCMSNoTuningLib STATIC ${CMAKE_CURRENT_BINARY_DIR}/include/SublimeCMSNoTuning.hpp)
set_target_properties(SublimeCMSNoTuningLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(SublimeCMSNoTuningLib PUBLIC ./include 
                                                        ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(SublimeCMSNoTuningLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

configure_file(./include/SublimeCMSNoTuningMorris.hpp.in ./include/SublimeCMSNoTuningMorris.hpp @ONLY)
add_library(SublimeCMSNoTuningMorrisLib STATIC ${CMAKE_CURRENT_BINARY_DIR}/include/SublimeCMSNoTuningMorris.hpp)
set_target_properties(SublimeCMSNoTuningMorrisLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(SublimeCMSNoTuningMorrisLib PUBLIC ./include 
                                                              ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(SublimeCMSNoTuningMorrisLib PUBLIC ${BITHACKING_COMPILE_FLAGS})


add_library(CSLib STATIC ./include/CS.hpp)
set_target_properties(CSLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSLib PUBLIC ./include)
target_compile_options(CSLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(SublimeCSLib STATIC ./include/SublimeCS.hpp)
set_target_properties(SublimeCSLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(SublimeCSLib PUBLIC ./include)
target_compile_options(SublimeCSLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

configure_file(./include/SublimeCSNoTuning.hpp.in ./include/SublimeCSNoTuning.hpp @ONLY)
add_library(SublimeCSNoTuningLib STATIC ${CMAKE_CURRENT_BINARY_DIR}/include/SublimeCSNoTuning.hpp)
set_target_properties(SublimeCSNoTuningLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(SublimeCSNoTuningLib PUBLIC ./include 
                                                       ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(SublimeCSNoTuningLib PUBLIC ${BITHACKING_COMPILE_FLAGS})



add_library(FingerprintTableLib STATIC ./include/FingerprintTable.hpp)
set_target_properties(FingerprintTableLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(FingerprintTableLib PUBLIC ./include)
target_compile_options(FingerprintTableLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(VALECountersLib STATIC ./include/VALECounters.hpp)
set_target_properties(VALECountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(VALECountersLib PUBLIC ./include)
target_compile_options(VALECountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(SublimeMGLib STATIC ./include/SublimeMG.hpp)
set_target_properties(SublimeMGLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(SublimeMGLib PUBLIC ./include)
target_compile_options(SublimeMGLib PUBLIC ${BITHACKING_COMPILE_FLAGS})


add_library(MGDummyLib STATIC ./include/MGDummy.hpp)
set_target_properties(MGDummyLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(MGDummyLib  PUBLIC ./include)
target_compile_options(MGDummyLib PUBLIC ${BITHACKING_COMPILE_FLAGS})


if (BUILD_TESTS)
    message(STATUS "Building tests")
    enable_testing()
    add_subdirectory(tests)
endif ()

if (BUILD_EXAMPLES)
    message(STATUS "Building examples")
    add_subdirectory(examples)
endif ()

if (BUILD_BENCHMARKS)
    if (NOT EXISTS ../bench/sketches_benchmark/include/bitsense/README.md)
        message(STATUS "Cloning baselines")
        execute_process(COMMAND bash -c "cd ../bench/scripts && bash setup_includes.sh")
    endif ()

    message(STATUS "Building benchmarks")
    add_subdirectory(bench)
endif ()

