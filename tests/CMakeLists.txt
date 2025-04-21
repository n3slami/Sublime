# doctest
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

add_executable(CMSketchbookFixedCountersTests ./CMSketchbookFixedCountersTests.cpp)
target_link_libraries(CMSketchbookFixedCountersTests CMSketchbookFixedCountersLib doctest)
add_test(NAME test_sb_cm_fixed COMMAND CMSketchbookFixedCountersTests)

add_executable(CMSketchbookSemiAdaptiveCountersTests ./CMSketchbookSemiAdaptiveTests.cpp)
target_link_libraries(CMSketchbookSemiAdaptiveCountersTests CMSketchbookSemiAdaptiveCountersLib doctest)
add_test(NAME test_sb_cm_semiada COMMAND CMSketchbookSemiAdaptiveCountersTests)

add_executable(CMSketchbookAdaptiveCountersTests ./CMSketchbookAdaptiveCountersTests.cpp)
target_link_libraries(CMSketchbookAdaptiveCountersTests CMSketchbookAdaptiveCountersLib doctest)
add_test(NAME test_sb_cm_ada COMMAND CMSketchbookAdaptiveCountersTests)


add_executable(CSketchbookFixedCountersTests ./CSketchbookFixedCountersTests.cpp)
target_link_libraries(CSketchbookFixedCountersTests CSketchbookFixedCountersLib doctest)
add_test(NAME test_sb_c_fixed COMMAND CSketchbookFixedCountersTests)

add_executable(CSketchbookSemiAdaptiveCountersTests ./CSketchbookSemiAdaptiveTests.cpp)
target_link_libraries(CSketchbookSemiAdaptiveCountersTests CSketchbookFixedCountersLib doctest)
add_test(NAME test_sb_c_semiada COMMAND CSketchbookSemiAdaptiveCountersTests)

add_executable(CSketchbookAdaptiveCountersTests ./CSketchbookAdaptiveCountersTests.cpp)
target_link_libraries(CSketchbookAdaptiveCountersTests CSketchbookAdaptiveCountersLib doctest)
add_test(NAME test_sb_c_ada COMMAND CSketchbookAdaptiveCountersTests)


add_test(NAME test_all COMMAND CMSketchbookFixedCountersTests 
                               CMSketchbookSemiAdaptiveCountersTests
                               CMSketchbookAdaptiveCountersTests
                               CSketchbookFixedCountersTests
                               CSketchbookSemiAdaptiveCountersTests
                               CSketchbookAdaptiveCountersTests)

