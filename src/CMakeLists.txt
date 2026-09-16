add_library(kestrel_core STATIC
    oms/order_pool.cpp
)

target_include_directories(kestrel_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

add_executable(kestrel_oms process_a_main.cpp)
target_link_libraries(kestrel_oms PRIVATE kestrel_core)

add_executable(kestrel_venue process_b_main.cpp)
target_link_libraries(kestrel_venue PRIVATE kestrel_core)
