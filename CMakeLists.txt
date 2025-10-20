cmake_minimum_required(VERSION 3.16.3)
project(Sketchbook
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

add_library(CMSketchbookFixedCountersLib STATIC ./include/CMSketchbookFixedCounters.hpp)
set_target_properties(CMSketchbookFixedCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookFixedCountersLib PUBLIC ./include)
target_compile_options(CMSketchbookFixedCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CMSketchbookAdaptiveCountersPQLib STATIC ./include/CMSketchbookAdaptiveCountersPQ.hpp)
set_target_properties(CMSketchbookAdaptiveCountersPQLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookAdaptiveCountersPQLib PUBLIC ./include)
target_compile_options(CMSketchbookAdaptiveCountersPQLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

configure_file(./include/CMSketchbookAdaptiveCountersPQNoTuning.hpp.in ./include/CMSketchbookAdaptiveCountersPQNoTuning.hpp @ONLY)
add_library(CMSketchbookAdaptiveCountersPQNoTuningLib STATIC ${CMAKE_CURRENT_BINARY_DIR}/include/CMSketchbookAdaptiveCountersPQNoTuning.hpp)
set_target_properties(CMSketchbookAdaptiveCountersPQNoTuningLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookAdaptiveCountersPQNoTuningLib PUBLIC ./include 
                                                                            ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(CMSketchbookAdaptiveCountersPQNoTuningLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

configure_file(./include/CMSketchbookAdaptiveCountersPQNoTuningMorris.hpp.in ./include/CMSketchbookAdaptiveCountersPQNoTuningMorris.hpp @ONLY)
add_library(CMSketchbookAdaptiveCountersPQNoTuningMorrisLib STATIC ${CMAKE_CURRENT_BINARY_DIR}/include/CMSketchbookAdaptiveCountersPQNoTuningMorris.hpp)
set_target_properties(CMSketchbookAdaptiveCountersPQNoTuningMorrisLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CMSketchbookAdaptiveCountersPQNoTuningMorrisLib PUBLIC ./include
                                                                                  ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(CMSketchbookAdaptiveCountersPQNoTuningMorrisLib PUBLIC ${BITHACKING_COMPILE_FLAGS})


add_library(CSketchbookFixedCountersLib STATIC ./include/CSketchbookFixedCounters.hpp)
set_target_properties(CSketchbookFixedCountersLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSketchbookFixedCountersLib PUBLIC ./include)
target_compile_options(CSketchbookFixedCountersLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

add_library(CSketchbookAdaptiveCountersPQLib STATIC ./include/CSketchbookAdaptiveCountersPQ.hpp)
set_target_properties(CSketchbookAdaptiveCountersPQLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSketchbookAdaptiveCountersPQLib PUBLIC ./include)
target_compile_options(CSketchbookAdaptiveCountersPQLib PUBLIC ${BITHACKING_COMPILE_FLAGS})

configure_file(./include/CSketchbookAdaptiveCountersPQNoTuning.hpp.in ./include/CSketchbookAdaptiveCountersPQNoTuning.hpp @ONLY)
add_library(CSketchbookAdaptiveCountersPQNoTuningLib STATIC ${CMAKE_CURRENT_BINARY_DIR}/include/CSketchbookAdaptiveCountersPQNoTuning.hpp)
set_target_properties(CSketchbookAdaptiveCountersPQNoTuningLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(CSketchbookAdaptiveCountersPQNoTuningLib PUBLIC ./include
                                                                           ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(CSketchbookAdaptiveCountersPQNoTuningLib PUBLIC ${BITHACKING_COMPILE_FLAGS})



add_library(MGDummyLib STATIC ./include/MGDummy.hpp)
set_target_properties(MGDummyLib PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(MGDummyLib  PUBLIC ./include)
target_compile_options(MGDummyLib PUBLIC ${BITHACKING_COMPILE_FLAGS})


#if (BUILD_TESTS)
#    message(STATUS "Building tests")
#    add_subdirectory(tests)
#endif ()

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

