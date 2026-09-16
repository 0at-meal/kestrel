add_executable(kestrel_benchmarks
    placeholder_benchmark.cpp
)

target_link_libraries(kestrel_benchmarks PRIVATE
    kestrel_core
    benchmark::benchmark
)
