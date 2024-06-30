add_executable(CMSketchbookFixedCountersTests ./CMSketchbookFixedCountersTests.cpp)
target_link_libraries(CMSketchbookFixedCountersTests CMSketchbookFixedCountersLib)
add_test(NAME test_sb_cm_fixed COMMAND CMSketchbookFixedCountersTests)

add_executable(CMSketchbookSemiAdaptiveCountersTests ./CMSketchbookSemiAdaptiveTests.cpp)
target_link_libraries(CMSketchbookSemiAdaptiveCountersTests CMSketchbookSemiAdaptiveCountersLib)
add_test(NAME test_sb_cm_semiada COMMAND CMSketchbookSemiAdaptiveCountersTests)

add_executable(CMSketchbookAdaptiveCountersTests ./CMSketchbookAdaptiveCountersTests.cpp)
target_link_libraries(CMSketchbookAdaptiveCountersTests CMSketchbookAdaptiveCountersLib)
add_test(NAME test_sb_cm_ada COMMAND CMSketchbookAdaptiveCountersTests)


add_executable(CSketchbookFixedCountersTests ./CSketchbookFixedCountersTests.cpp)
target_link_libraries(CSketchbookFixedCountersTests CSketchbookFixedCountersLib)
add_test(NAME test_sb_c_fixed COMMAND CSketchbookFixedCountersTests)

add_executable(CSketchbookSemiAdaptiveCountersTests ./CSketchbookSemiAdaptiveTests.cpp)
target_link_libraries(CSketchbookSemiAdaptiveCountersTests CSketchbookFixedCountersLib)
add_test(NAME test_sb_c_semiada COMMAND CSketchbookSemiAdaptiveCountersTests)

add_executable(CSketchbookAdaptiveCountersTests ./CSketchbookAdaptiveCountersTests.cpp)
target_link_libraries(CSketchbookAdaptiveCountersTests CSketchbookAdaptiveCountersLib)
add_test(NAME test_sb_c_ada COMMAND CSketchbookAdaptiveCountersTests)


add_test(NAME test_all COMMAND CMSketchbookFixedCountersTests 
                               CMSketchbookSemiAdaptiveCountersTests
                               CMSketchbookAdaptiveCountersTests
                               CSketchbookFixedCountersTests
                               CSketchbookSemiAdaptiveCountersTests
                               CSketchbookAdaptiveCountersTests)

