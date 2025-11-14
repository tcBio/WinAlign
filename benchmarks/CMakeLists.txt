# Benchmarks for WinAlign-GPU

add_executable(winalign_bench
    bench_main.cpp
    bench_seeding.cpp
    bench_alignment.cpp
)

target_link_libraries(winalign_bench
    PRIVATE
        winalign
)

target_include_directories(winalign_bench
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)
