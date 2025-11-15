# WinAlign-Amplicon CMake Configuration

# CUDA kernels for amplicon processing
add_library(winalign_amplicon_cuda STATIC
    cuda/pileup.cu
    cuda/variant_calling.cu
    cuda/memory_pool.cu
    cuda/multi_gpu.cu
)

target_include_directories(winalign_amplicon_cuda PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(winalign_amplicon_cuda PUBLIC
    CUDA::cudart
    CUDA::cuda_driver
)

set_target_properties(winalign_amplicon_cuda PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
)

# Amplicon library (static) - CPU components
add_library(winalign_amplicon STATIC
    read_collapser.cpp
    amplicon_assigner.cpp
    config_parser.cpp
    vcf_writer.cpp
    variant_caller.cpp
    amplicon_pipeline.cpp
    smith_waterman.cpp
    primer_trimmer.cpp
    chimera_detector.cpp
    barcode_demux.cpp
    multi_sample_vcf.cpp
    realtime_processor.cpp
    simd_ops.cpp
    performance_monitor.cpp
)

# Enable SIMD optimizations if supported
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
if(COMPILER_SUPPORTS_AVX2)
    target_compile_options(winalign_amplicon PRIVATE -mavx2 -mfma)
    message(STATUS "AVX2 support enabled for amplicon library")
endif()

target_include_directories(winalign_amplicon PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(winalign_amplicon PUBLIC
    winalign_cpu
    winalign_amplicon_cuda
)

# Amplicon executable
add_executable(winalign-amplicon
    main.cpp
)

target_link_libraries(winalign-amplicon PRIVATE
    winalign_amplicon
    winalign_amplicon_cuda
    winalign_cpu
    CUDA::cudart
)

# Set output directory
set_target_properties(winalign-amplicon PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Install targets
install(TARGETS winalign-amplicon
    RUNTIME DESTINATION bin
)

install(TARGETS winalign_amplicon
    ARCHIVE DESTINATION lib
)
