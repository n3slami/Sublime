add_executable(CMSketchbookFixedCountersTests ./CMSketchbookFixedCountersTests.cpp)
target_link_libraries(CMSketchbookFixedCountersTests CMSketchbookFixedCountersLib)
add_test(NAME test_sb_cm_fixed COMMAND CMSketchbookFixedCountersTests)

add_executable(CMSketchbookSemiAdaptiveCountersTests ./CMSketchbookSemiAdaptiveTests.cpp)
target_link_libraries(CMSketchbookSemiAdaptiveCountersTests CMSketchbookSemiAdaptiveCountersLib)
add_test(NAME test_sb_cm_semiada COMMAND CMSketchbookSemiAdaptiveCountersTests)

add_executable(CMSketchbookAdaptiveCountersTests ./CMSketchbookAdaptiveCountersTests.cpp)
target_link_libraries(CMSketchbookAdaptiveCountersTests CMSketchbookAdaptiveCountersLib)
add_test(NAME test_sb_cm_ada COMMAND CMSketchbookAdaptiveCountersTests)

add_test(NAME test_all COMMAND CMSketchbookFixedCountersTests 
                               CMSketchbookSemiAdaptiveCountersTests
                               CMSketchbookAdaptiveCountersTests)

