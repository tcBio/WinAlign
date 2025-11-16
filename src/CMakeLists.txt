# Core library components
add_subdirectory(core)
add_subdirectory(cpu)
add_subdirectory(cuda)

# Multi-process utilities (Phase 5)
add_subdirectory(utils)

# Amplicon tool (optional)
if(BUILD_AMPLICON)
    add_subdirectory(amplicon)
endif()

# Main WinAlign library
add_library(winalign STATIC
    $<TARGET_OBJECTS:winalign_core>
    $<TARGET_OBJECTS:winalign_cpu>
    $<TARGET_OBJECTS:winalign_cuda>
)

target_link_libraries(winalign
    PUBLIC
        CUDA::cudart
        CUDA::cuda_driver
        ZLIB::ZLIB
        divsufsort64
)

target_include_directories(winalign
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Main executables
add_executable(winalign-gpu main.cpp)
target_link_libraries(winalign-gpu PRIVATE winalign)

# Optimized GPU executable (identical code, different binary name)
add_executable(winalign-gpu-fast main.cpp)
target_link_libraries(winalign-gpu-fast PRIVATE winalign)

# Install
install(TARGETS winalign winalign-gpu winalign-gpu-fast
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
