cmake_minimum_required(VERSION 3.16.3)
project(Sketchbook
        VERSION 1.0
        HOMEPAGE_URL https://github.com/n3slami/Sketchbook)

option(BUILD_TESTS "Build the tests" ON)
option(BUILD_EXAMPLES "Build the examples" ON)
option(BUILD_BENCHMARKS "Build the benchmark targets" ON)

set(CMAKE_CXX_STANDARD 17)
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
else()
    set(USE_MULTI_THREADED OFF)
endif ()

set(BITHACKING_COMPILE_FLAGS -Ofast -march=native)
#set(BITHACKING_COMPILE_FLAGS -Ofast -mbmi2 -msse4.2 -D__SSE4_2_)

add_library(CMSketchbookFixedCountersLib STATIC ./include/CMSketchbookFixedCounters.hpp)
set_target_properties(CMSketchbookFixedCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookFixedCountersLib PUBLIC ./include)
target_compile_options(CMSketchbookFixedCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CMSketchbookSemiAdaptiveCountersLib STATIC ./include/CMSketchbookSemiAdaptiveCounters.hpp)
set_target_properties(CMSketchbookSemiAdaptiveCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookSemiAdaptiveCountersLib PUBLIC ./include)
target_compile_options(CMSketchbookSemiAdaptiveCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CMSketchbookAdaptiveCountersLib STATIC ./include/CMSketchbookAdaptiveCounters.hpp)
set_target_properties(CMSketchbookAdaptiveCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookAdaptiveCountersLib PUBLIC ./include)
target_compile_options(CMSketchbookAdaptiveCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CMSketchbookAdaptiveCountersPQLib STATIC ./include/CMSketchbookAdaptiveCountersPQ.hpp)
set_target_properties(CMSketchbookAdaptiveCountersPQLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookAdaptiveCountersPQLib PUBLIC ./include)
target_compile_options(CMSketchbookAdaptiveCountersPQLib PUBLIC ${BITHACKING_COMPILE_FLAGS})



add_library(CSketchbookFixedCountersLib STATIC ./include/CSketchbookFixedCounters.hpp)
set_target_properties(CSketchbookFixedCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSketchbookFixedCountersLib PUBLIC ./include)
target_compile_options(CSketchbookFixedCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CSketchbookSemiAdaptiveCountersLib STATIC ./include/CSketchbookSemiAdaptiveCounters.hpp)
set_target_properties(CSketchbookSemiAdaptiveCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSketchbookSemiAdaptiveCountersLib PUBLIC ./include)
target_compile_options(CSketchbookSemiAdaptiveCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CSketchbookAdaptiveCountersLib STATIC ./include/CSketchbookAdaptiveCounters.hpp)
set_target_properties(CSketchbookAdaptiveCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSketchbookAdaptiveCountersLib PUBLIC ./include)
target_compile_options(CSketchbookAdaptiveCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CSketchbookAdaptiveCountersPQLib STATIC ./include/CSketchbookAdaptiveCountersPQ.hpp)
set_target_properties(CSketchbookAdaptiveCountersPQLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSketchbookAdaptiveCountersPQLib PUBLIC ./include)
target_compile_options(CSketchbookAdaptiveCountersPQLib PUBLIC ${BITHACKING_COMPILE_FLAGS})


if (BUILD_TESTS)
    message(STATUS "Building tests")
    add_subdirectory(tests)
endif ()

if (BUILD_EXAMPLES)
    message(STATUS "Building examples")
    add_subdirectory(examples)
endif ()

if (BUILD_BENCHMARKS)
    message(STATUS "Building benchmarks")
    add_subdirectory(bench)
endif ()

