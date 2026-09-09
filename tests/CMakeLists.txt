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
add_executable(FingerprintTableTests ./FingerprintTableTests.cpp)
target_link_libraries(FingerprintTableTests FingerprintTableLib doctest)
add_test(NAME fingerprint_table COMMAND FingerprintTableTests)

add_executable(VALECountersTests ./VALECountersTests.cpp)
target_link_libraries(VALECountersTests VALECountersLib doctest)
add_test(NAME vale_counters COMMAND VALECountersTests)

add_executable(SublimeMGTests ./SublimeMGTests.cpp)
target_link_libraries(SublimeMGTests SublimeMGLib doctest)
add_test(NAME sublime_mg COMMAND SublimeMGTests)


# All
add_test(NAME all COMMAND CMSTests SublimeCMSTests 
                          CSTests  SublimeCSTests
                          FingerprintTableTests
                          VALECountersTests SublimeMGTests)

