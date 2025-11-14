# CPU-side components
add_library(winalign_cpu OBJECT
    fastq_parser.cpp
    reference_loader.cpp
    bam_writer.cpp
    fm_index.cpp
)

target_include_directories(winalign_cpu
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(winalign_cpu
    PUBLIC
        ZLIB::ZLIB
)
