# Unit tests for WinAlign-GPU

# Find Google Test (or create simple test framework)
# For now, we'll use a simple test framework

add_executable(winalign_tests
    test_main.cpp
    test_fastq_parser.cpp
    test_reference_loader.cpp
    test_fm_index.cpp
)

target_link_libraries(winalign_tests
    PRIVATE
        winalign
)

target_include_directories(winalign_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# Add tests to CTest
add_test(NAME WinAlignTests COMMAND winalign_tests)
