include(GoogleTest)

add_executable(kestrel_tests
    placeholder_test.cpp
)

target_link_libraries(kestrel_tests PRIVATE
    kestrel_core
    GTest::gtest_main
)

gtest_discover_tests(kestrel_tests)
