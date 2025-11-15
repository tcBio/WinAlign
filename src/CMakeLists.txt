# Core library components
add_subdirectory(core)
add_subdirectory(cpu)
add_subdirectory(cuda)

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
)

target_include_directories(winalign
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Main executable
add_executable(winalign-gpu main.cpp)
target_link_libraries(winalign-gpu PRIVATE winalign)

# Install
install(TARGETS winalign winalign-gpu
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
