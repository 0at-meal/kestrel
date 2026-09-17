add_library(kestrel_core STATIC
    gateway/inbound_gateway.cpp
    gateway/outbound_gateway.cpp
    oms/order_pool.cpp
    perf/latency_histogram.cpp
    venue/stub_venue.cpp
)

target_include_directories(kestrel_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
    ${QuickFIX_INCLUDE_DIRS}
)

target_link_directories(kestrel_core PUBLIC
    ${QuickFIX_LIBRARY_DIRS}
)

target_link_libraries(kestrel_core PUBLIC
    ${QuickFIX_LIBRARIES}
    z
    m
)

add_executable(kestrel_oms process_a_main.cpp)
target_link_libraries(kestrel_oms PRIVATE kestrel_core)

add_executable(kestrel_venue process_b_main.cpp)
target_link_libraries(kestrel_venue PRIVATE kestrel_core)
