# WinAlign-Amplicon CMake Configuration

# Amplicon library (static)
add_library(winalign_amplicon STATIC
    read_collapser.cpp
    amplicon_assigner.cpp
    config_parser.cpp
    amplicon_pipeline.cpp
)

target_include_directories(winalign_amplicon PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(winalign_amplicon PUBLIC
    winalign_cpu
)

# Amplicon executable
add_executable(winalign-amplicon
    main.cpp
)

target_link_libraries(winalign-amplicon PRIVATE
    winalign_amplicon
    winalign_cpu
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
