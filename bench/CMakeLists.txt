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

list(APPEND Targets "CMSketchbookFixedCounters" "CMSketchbookAdaptiveCountersPQ" "CMSketchbookAdaptiveCountersPQNoTuning"
                    "StingyCM"
                    "CodingCM"
                    "BitSenseCM"
                    "SEADCM"
                    "SALSACM"
                    "CSketchbookFixedCounters" "CSketchbookAdaptiveCountersPQ" "CSketchbookAdaptiveCountersPQNoTuning"
                    "StingyC"
                    "CodingC"
                    "BitSenseC"
                    "SEADC"
                    "Switch"
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

    if (ds MATCHES "CMSketchbookFixedCounters")
        target_link_libraries(bench_${ds} CMSketchbookFixedCountersLib)
    elseif (ds MATCHES "CMSketchbookAdaptiveCountersPQ")
        target_link_libraries(bench_${ds} CMSketchbookAdaptiveCountersPQLib)
    elseif (ds MATCHES "CMSketchbookAdaptiveCountersPQNoTuning")
        target_link_libraries(bench_${ds} CMSketchbookAdaptiveCountersPQNoTuningLib)
    elseif (ds MATCHES "StingyCM")
        add_library(StingyCMLib STATIC ./sketches_benchmark/include/StingyCM.hpp 
                                       ./sketches_benchmark/include/MurmurHash.h 
                                       ./sketches_benchmark/include/params/stingy.h)
        set_target_properties(StingyCMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(StingyCMLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} StingyCMLib)
    elseif (ds MATCHES "CodingCM")
        add_library(CodingCMLib STATIC ./sketches_benchmark/include/CodingCM.hpp 
                                       ./sketches_benchmark/include/MurmurHash.h 
                                       ./sketches_benchmark/include/params/coding.h)
        set_target_properties(CodingCMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(CodingCMLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} CodingCMLib)
    elseif (ds MATCHES "BitSenseCM")
        add_library(BitSenseCMLib STATIC ./sketches_benchmark/include/bitsense/simulator/src/sketch/BSCMSketch.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/bitsense.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/sketch.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/flowkey.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/data.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/hash.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/test.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/utils.h
                                         ./sketches_benchmark/include/bitsense/simulator/src/common/logger.h)
        set_target_properties(BitSenseCMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(BitSenseCMLib PUBLIC ${BITHACKING_COMPILE_FLAGS} -mno-avx512f)
        target_link_libraries(bench_${ds} BitSenseCMLib BitSenseOmniTools)
    elseif (ds MATCHES "SEADCM")
        add_library(SEADCMLib STATIC ./sketches_benchmark/include/sead_counter/SEAD_Counter_Sketch/cmsketch.h
                                     ./sketches_benchmark/include/sead_counter/SEAD_Counter_Sketch/SmallActiveCounter.h)
        target_include_directories(SEADCMLib PUBLIC ./sketches_benchmark/include/sead_counter/SEAD_Counter_Sketch)
        target_compile_definitions(SEADCMLib PUBLIC COUNTER_SIZE=12)
        target_compile_definitions(SEADCMLib PUBLIC per_estimator_int=12)
        target_compile_definitions(SEADCMLib PUBLIC per_estimator_bit=12.5)
        set_target_properties(SEADCMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(SEADCMLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} SEADCMLib)
    elseif (ds MATCHES "SALSACM")
        add_library(SALSACMLib STATIC ./sketches_benchmark/include/salsa/Salsa/SalsaCMS.hpp
                                      ./sketches_benchmark/include/salsa/Salsa/SalsaCMS.cpp
                                      ./sketches_benchmark/include/salsa/Salsa/BobHash.hpp
                                      ./sketches_benchmark/include/salsa/Salsa/BobHash.cpp)
        target_include_directories(SALSACMLib PUBLIC ./sketches_benchmark/include/salsa/Salsa/)
        set_target_properties(SALSACMLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(SALSACMLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} SALSACMLib)
    elseif (ds MATCHES "CSketchbookFixedCounters")
        target_link_libraries(bench_${ds} CSketchbookFixedCountersLib)
    elseif (ds MATCHES "CSketchbookAdaptiveCountersPQ")
        target_link_libraries(bench_${ds} CSketchbookAdaptiveCountersPQLib)
    elseif (ds MATCHES "CSketchbookAdaptiveCountersPQNoTuning")
        target_link_libraries(bench_${ds} CSketchbookAdaptiveCountersPQNoTuningLib)
    elseif (ds MATCHES "StingyC")
        add_library(StingyCLib STATIC ./sketches_benchmark/include/StingyC.hpp 
                                      ./sketches_benchmark/include/MurmurHash.h 
                                      ./sketches_benchmark/include/params/stingy.h)
        set_target_properties(StingyCLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(StingyCLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} StingyCLib)
    elseif (ds MATCHES "CodingC")
        add_library(CodingCLib STATIC ./sketches_benchmark/include/CodingC.hpp
                                      ./sketches_benchmark/include/MurmurHash.h 
                                      ./sketches_benchmark/include/params/coding.h)
        set_target_properties(CodingCLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(CodingCLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} CodingCLib)
    elseif (ds MATCHES "BitSenseC")
        add_library(BitSenseCLib STATIC ./sketches_benchmark/include/bitsense/simulator/src/sketch/BSCountSketch.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/bitsense.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/sketch.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/flowkey.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/data.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/hash.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/test.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/utils.h
                                        ./sketches_benchmark/include/bitsense/simulator/src/common/logger.h)
        set_target_properties(BitSenseCLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(BitSenseCLib PUBLIC ${BITHACKING_COMPILE_FLAGS} -mno-avx512f)
        target_link_libraries(bench_${ds} BitSenseCLib BitSenseOmniTools)
    elseif (ds MATCHES "SEADC")
        add_library(SEADCLib STATIC ./sketches_benchmark/include/sead_counter/SEAD_Counter_Sketch/csketch.h)
        target_include_directories(SEADCLib PUBLIC ./sketches_benchmark/include/sead_counter/SEAD_Counter_Sketch)
        target_compile_definitions(SEADCLib PUBLIC COUNTER_SIZE=12)
        target_compile_definitions(SEADCLib PUBLIC per_estimator_int=12)
        target_compile_definitions(SEADCLib PUBLIC per_estimator_bit=12.5)
        set_target_properties(SEADCLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(SEADCLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} SEADCLib)
    elseif (ds MATCHES "Switch")
        add_library(SwitchLib STATIC ./sketches_benchmark/include/Switch.hpp)
        set_target_properties(SwitchLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(SwitchLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} SwitchLib)
    elseif (ds MATCHES ".*Tailored")
        add_library(${ds}Lib STATIC ./sketches_benchmark/include/${ds}.hpp)
        set_target_properties(${ds}Lib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(${ds}Lib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} ${ds}Lib)
    elseif (ds MATCHES "Waving")
        add_library(WavingLib STATIC ./sketches_benchmark/include/waving_sketch/include/Waving.h ./sketches_benchmark/include/waving_sketch/include/bitset.h)
        target_include_directories(WavingLib PUBLIC ./sketches_benchmark/include/waving_sketch/include/)
        set_target_properties(WavingLib PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_options(WavingLib PUBLIC ${BITHACKING_COMPILE_FLAGS})
        target_link_libraries(bench_${ds} WavingLib)
    elseif (ds MATCHES "MGDummy")
        target_link_libraries(bench_${ds} MGDummyLib)
    endif()
endfunction(compile_bench)

add_subdirectory(./sketches_benchmark/include/bitsense/simulator/third_party/eigen)
add_subdirectory(./sketches_benchmark/include/bitsense/simulator/third_party/fmt)
add_subdirectory(./sketches_benchmark/include/bitsense/simulator/third_party/toml)
# Configuring tomlplusplus & eigen needs a bit more work
include_directories(./sketches_benchmark/include/bitsense/simulator/third_party/toml/include)
include_directories(./sketches_benchmark/include/bitsense/simulator/third_party/eigen)

find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

add_library(BitSenseOmniTools ./sketches_benchmark/include/bitsense/simulator/src/impl/utils.cpp
                              ./sketches_benchmark/include/bitsense/simulator/src/impl/logger.cpp
                              ./sketches_benchmark/include/bitsense/simulator/src/impl/data.cpp
                              ./sketches_benchmark/include/bitsense/simulator/src/impl/test.cpp
                              ./sketches_benchmark/include/bitsense/simulator/src/impl/hash.cpp)
target_include_directories(BitSenseOmniTools PUBLIC ./sketches_benchmark/include/bitsense/simulator/src/)
target_link_libraries(BitSenseOmniTools fmt)

foreach(ds ${Targets})
    compile_bench(${ds})
endforeach()

# Setup workload generation binary
add_executable(workload_gen workload_gen.cpp zipf/zipf.h zipf/zipf.c)
target_link_libraries(workload_gen argparse)
