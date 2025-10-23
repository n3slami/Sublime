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

list(APPEND Targets "SublimeCMS" "SublimeCMSNoTuning" "SublimeCMSNoTuningMorris"
                    "CMS"
                    "StingyCM"
                    "CodingCM"
                    "SALSACM"
                    "SublimeCS" "SublimeCSNoTuning"
                    "CS"
                    "StingyC"
                    "CodingC"
                    "Tailored"
                    "OTailored"
                    "Waving"
                    "MGDummy")

function(compile_bench ds)
    if (NOT ds IN_LIST Targets)
        message(WARNING "${ds} is not defined")
        return()
    endif()

    add_executable(bench_${ds} sketches_benchmark/bench_${ds}.cpp)
    set_target_properties(bench_${ds} PROPERTIES LINKER_LANGUAGE CXX)
    target_link_libraries(bench_${ds} argparse)

    if (ds STREQUAL "SublimeCMS")
        target_link_libraries(bench_${ds} SublimeCMSLib)
    elseif (ds STREQUAL "SublimeCMSNoTuning")
        target_link_libraries(bench_${ds} SublimeCMSNoTuningLib)
    elseif (ds STREQUAL "SublimeCMSNoTuningMorris")
        target_link_libraries(bench_${ds} SublimeCMSNoTuningMorrisLib)
    elseif (ds STREQUAL "CMS")
        target_link_libraries(bench_${ds} CMSLib)
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
    elseif (ds STREQUAL "SublimeCS")
        target_link_libraries(bench_${ds} SublimeCSLib)
    elseif (ds STREQUAL "SublimeCSNoTuning")
        target_link_libraries(bench_${ds} SublimeCSNoTuningLib)
    elseif (ds STREQUAL "CS")
        target_link_libraries(bench_${ds} CSLib)
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
        target_compile_options(WavingLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} WavingLib)
    elseif (ds STREQUAL "MGDummy")
        target_link_libraries(bench_${ds} MGDummyLib)
    endif()
endfunction(compile_bench)

foreach(ds ${Targets})
    compile_bench(${ds})
endforeach()

# Setup workload generation binary
add_executable(workload_gen workload_gen.cpp zipf/zipf.h zipf/zipf.c)
target_link_libraries(workload_gen argparse)
