# Multi-process orchestration utilities (Phase 5)

# FASTQ chunker utility
add_executable(winalign-chunk fastq_chunker.cpp)
target_link_libraries(winalign-chunk PRIVATE ZLIB::ZLIB)

# Process coordinator utility
add_executable(winalign-multi process_coordinator.cpp)
if(UNIX)
    target_link_libraries(winalign-multi PRIVATE pthread)
endif()

# BAM merger utility
add_executable(winalign-merge bam_merger.cpp)

# Install utilities
install(TARGETS winalign-chunk winalign-multi winalign-merge
    RUNTIME DESTINATION bin
)
