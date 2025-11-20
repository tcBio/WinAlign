/**
 * @file pipeline_gpu_context.h
 * @brief GPU context lifecycle management for multi-stream pipeline
 */

#pragma once

#include "pipeline_internal.h"
#include <vector>
#include <cstddef>

namespace winalign {

// Forward declarations
struct PipelineConfig;

namespace cuda {
class MemoryManager;
}

namespace internal {

/**
 * @brief Manages allocation, initialization, and cleanup of GPU batch contexts
 *
 * This class handles the lifecycle of NUM_GPU_CONTEXTS GPU contexts used for
 * multi-stream async pipeline processing. Each context has its own CUDA stream,
 * events, and device/host buffers for overlapping computation and data transfer.
 */
class GpuContextManager {
public:
    /**
     * @brief Construct GPU context manager
     * @param config Pipeline configuration
     * @param mem_manager CUDA memory manager for allocations
     * @param gpu_enabled Whether GPU processing is enabled
     */
    GpuContextManager(
        const PipelineConfig& config,
        cuda::MemoryManager* mem_manager,
        bool gpu_enabled);

    /**
     * @brief Destructor - ensures cleanup is called
     */
    ~GpuContextManager();

    /**
     * @brief Initialize all GPU contexts
     *
     * Allocates NUM_GPU_CONTEXTS contexts, each with:
     * - Dedicated CUDA stream
     * - 4 CUDA events (H2D, seeding, SW, D2H)
     * - Device buffers for reads, seeds, and results
     * - Pinned host buffers for async transfers
     *
     * @return true if initialization succeeded, false otherwise
     */
    bool initialize();

    /**
     * @brief Clean up all GPU contexts
     *
     * Synchronizes all streams and frees all allocated resources.
     * Safe to call multiple times.
     */
    void cleanup();

    /**
     * @brief Get mutable reference to GPU contexts
     * @return Reference to vector of GPU batch contexts
     */
    std::vector<GpuBatchContext>& get_contexts();

    /**
     * @brief Get const reference to GPU contexts
     * @return Const reference to vector of GPU batch contexts
     */
    const std::vector<GpuBatchContext>& get_contexts() const;

    /**
     * @brief Check if contexts are initialized
     * @return true if initialized, false otherwise
     */
    bool is_initialized() const;

    /**
     * @brief Get number of contexts
     * @return Number of GPU contexts (typically NUM_GPU_CONTEXTS)
     */
    size_t num_contexts() const;

private:
    const PipelineConfig& config_;
    cuda::MemoryManager* mem_manager_;
    bool gpu_enabled_;
    bool initialized_;
    std::vector<GpuBatchContext> contexts_;
};

} // namespace internal
} // namespace winalign
