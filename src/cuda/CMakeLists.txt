# GPU kernels and CUDA components
add_library(winalign_cuda OBJECT
    memory_manager.cu
    seeding.cu
    seeding_chain.cu
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

# Build CUDA objects without separable compilation / RDC to avoid
# unresolved __cudaRegisterLinkedBinary symbols when linking the
# final executable with MSVC + CUDA 13.
