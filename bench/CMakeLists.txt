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

list(APPEND Targets "CMSketchbookFixedCounters" "CMSketchbookAdaptiveCounters" "CMSketchbookAdaptiveCountersPQ"
                    "StingyCM")

function(compile_bench ds)
    if (NOT ds IN_LIST Targets)
        message(WARNING "${ds} is not defined")
        return()
    endif()

    add_executable(bench_${ds} sketches_benchmark/bench_${ds}.cpp)
    set_target_properties(bench_${ds} PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(bench_${ds} argparse)

    if (ds MATCHES "CMSketchbookFixedCounters")
        target_link_libraries(bench_${ds} CMSketchbookFixedCountersLib)
    elseif (ds MATCHES "CMSketchbookAdaptiveCounters")
        target_link_libraries(bench_${ds} CMSketchbookAdaptiveCountersLib)
    elseif (ds MATCHES "CMSketchbookAdaptiveCountersPQ")
        target_link_libraries(bench_${ds} CMSketchbookAdaptiveCountersPQLib)
    elseif (ds MATCHES "StingyCM")
        add_library(StingyCMLib STATIC ./sketches_benchmark/include/StingyCM.hpp 
                                       ./sketches_benchmark/include/MurmurHash.h 
                                       ./sketches_benchmark/include/params.h)
        set_target_properties(StingyCMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_link_libraries(bench_${ds} StingyCMLib)
    endif()
endfunction(compile_bench)

foreach(ds ${Targets})
    compile_bench(${ds})
endforeach()

