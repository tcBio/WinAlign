# Core orchestration and coordination
add_library(winalign_core OBJECT
    pipeline.cpp
    pipeline_gpu_context.cpp
    pipeline_metrics.cpp
    pipeline_multistream.cpp
    pipeline_batch_helpers.cpp
    pipeline_initialization.cpp
    config.cpp
    memory_manager.cpp
    logger.cpp
)

target_include_directories(winalign_core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(winalign_core
    PUBLIC
        CUDA::cudart
)
