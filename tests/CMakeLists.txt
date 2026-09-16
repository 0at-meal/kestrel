include(GoogleTest)

add_executable(kestrel_tests
    placeholder_test.cpp
    wire/wire_structs_test.cpp
    core/spsc_ring_buffer_test.cpp
    core/spsc_ring_buffer_stress_test.cpp
    oms/order_state_test.cpp
    oms/order_state_machine_test.cpp
    oms/order_pool_test.cpp
    perf/latency_histogram_test.cpp
)

target_link_libraries(kestrel_tests PRIVATE
    kestrel_core
    GTest::gtest_main
)

gtest_discover_tests(kestrel_tests)
