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


# All
add_test(NAME all COMMAND CMSTests SublimeCMSTests 
                          CSTests  SublimeCSTests)

