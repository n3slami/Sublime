# doctest
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
)
FetchContent_MakeAvailable(doctest)

# CMS
add_executable(CMSTests ./CMSTests.cpp)
target_link_libraries(CMSTests CMSLib doctest)
add_test(NAME cms COMMAND CMSTests)

add_executable(SublimeCMSTests ./SublimeCMSTests.cpp)
target_link_libraries(SublimeCMSTests SublimeCMSLib CMSLib doctest)
add_test(NAME sublime_cms COMMAND SublimeCMSTests)


# CS
add_executable(CSTests ./CSTests.cpp)
target_link_libraries(CSTests CSLib doctest)
add_test(NAME cs COMMAND CSTests)

add_executable(SublimeCSTests ./SublimeCSTests.cpp)
target_link_libraries(SublimeCSTests SublimeCSLib CSLib doctest)
add_test(NAME sublime_cs COMMAND SublimeCSTests)


# Misra-Gries

add_executable(CuckooTableTests ./CuckooTableTests.cpp)
target_link_libraries(CuckooTableTests CuckooTableLib doctest)
add_test(NAME cuckoo_table COMMAND CuckooTableTests)

add_executable(VALECountersTests ./VALECountersTests.cpp)
target_link_libraries(VALECountersTests VALECountersLib doctest)
add_test(NAME vale_counters COMMAND VALECountersTests)

add_executable(SublimeMGTests ./SublimeMGTests.cpp)
target_link_libraries(SublimeMGTests SublimeMGLib doctest)
add_test(NAME sublime_mg COMMAND SublimeMGTests)

add_executable(MGTests ./MGTests.cpp)
target_link_libraries(MGTests MGLib doctest)
add_test(NAME mg COMMAND MGTests)

add_executable(SpaceSavingTests ./SpaceSavingTests.cpp)
target_link_libraries(SpaceSavingTests SpaceSavingLib doctest)
add_test(NAME space_saving COMMAND SpaceSavingTests)


# All
add_test(NAME all COMMAND CMSTests SublimeCMSTests
                          CSTests  SublimeCSTests
                          VALECountersTests SublimeMGTests
                          MGTests SpaceSavingTests CuckooTableTests)

