# GPU kernels and CUDA components
add_library(winalign_cuda OBJECT
    seeding.cu
    alignment.cu
    filtering.cu
)

target_include_directories(winalign_cuda
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(winalign_cuda
    PUBLIC
        CUDA::cudart
        CUDA::cuda_driver
)

# CUDA compilation flags
target_compile_options(winalign_cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:
        --use_fast_math
        --ptxas-options=-v
        -lineinfo
    >
)

# Enable separable compilation for CUDA
set_target_properties(winalign_cuda PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
)
