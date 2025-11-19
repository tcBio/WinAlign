/**
 * @file pipeline_initialization.h
 * @brief Pipeline initialization and resource allocation
 */

#pragma once

#include "winalign/common.h"
#include "winalign/result.h"
#include "winalign/cuda/fm_index.cuh"
#include "winalign/cuda/seeding.cuh"
#include "winalign/cuda/alignment.cuh"
#include <memory>
#include <functional>
#include <string>
#include <map>
#include <cuda_runtime.h>

namespace winalign {

// Forward declarations
struct PipelineConfig;
class ReferenceLoader;
class BamWriter;

namespace cuda {
class MemoryManager;
struct FMIndexView;
}

namespace internal {

// Forward declarations
class GpuContextManager;
struct ChromosomeOffset;

/**
 * @brief Handles pipeline initialization and resource allocation
 *
 * Manages the complex initialization sequence including reference loading,
 * FM-index building, GPU resource allocation, and BAM writer setup.
 */
class PipelineInitializer {
public:
    /**
     * @brief Construct pipeline initializer
     * @param config Pipeline configuration
     * @param update_progress_fn Progress callback function
     */
    PipelineInitializer(
        const PipelineConfig& config,
        std::function<void(double, const std::string&)> update_progress_fn);

    /**
     * @brief Initialize all pipeline resources
     * @return Result indicating success or failure
     */
    Result<bool> initialize();

    /**
     * @brief Cleanup all allocated resources
     */
    void cleanup();

    // Getters for initialized resources (not owned - just references)
    ReferenceLoader* get_reference_loader() { return reference_loader_.get(); }
    BamWriter* get_bam_writer() { return bam_writer_.get(); }
    cuda::MemoryManager* get_gpu_memory_manager() { return gpu_mem_manager_.get(); }
    GpuContextManager* get_gpu_context_manager() { return gpu_context_manager_.get(); }

    // GPU resource getters
    bool is_gpu_enabled() const { return gpu_enabled_; }
    const cuda::FMIndexView* get_fm_index_view() const { return fm_index_view_; }
    const char* get_device_reference() const { return d_reference_; }
    size_t get_reference_length() const { return reference_length_; }
    cuda::ReadBatch& get_device_read_batch() { return d_read_batch_; }
    cuda::Seed* get_device_seeds() { return d_seeds_; }
    cuda::AlignmentResult* get_device_results() { return d_results_; }
    cuda::FMIndex& get_device_fm_index() { return d_fm_index_; }
    cudaStream_t get_compute_stream() { return compute_stream_; }

    // Host resource getters
    const std::string* get_concatenated_reference() const { return concatenated_reference_; }
    const std::vector<ChromosomeOffset>& get_chromosome_offsets() const { return chromosome_offsets_; }

    // Pinned memory getters
    char* get_pinned_sequences() { return pinned_read_sequences_; }
    uint32_t* get_pinned_offsets() { return pinned_read_offsets_; }
    uint32_t* get_pinned_lengths() { return pinned_read_lengths_; }
    cuda::AlignmentResult* get_pinned_results() { return pinned_results_; }
    size_t get_pinned_sequences_capacity() const { return pinned_sequences_capacity_; }
    size_t get_pinned_reads_capacity() const { return pinned_reads_capacity_; }
    size_t get_pinned_results_capacity() const { return pinned_results_capacity_; }

private:
    /**
     * @brief Initialize reference genome and FM-index
     * @return Result indicating success or failure
     */
    Result<bool> initialize_reference();

    /**
     * @brief Initialize GPU resources
     * @return Result indicating success or failure
     */
    Result<bool> initialize_gpu();

    /**
     * @brief Initialize BAM writer
     * @return Result indicating success or failure
     */
    Result<bool> initialize_bam_writer();

    /**
     * @brief Allocate pinned host memory buffers
     * @return Result indicating success or failure
     */
    Result<bool> allocate_pinned_buffers();

    // Configuration
    const PipelineConfig& config_;
    std::function<void(double, const std::string&)> update_progress_fn_;

    // Reference resources (owned)
    std::unique_ptr<ReferenceLoader> reference_loader_;
    const cuda::FMIndexView* fm_index_view_;
    const std::string* concatenated_reference_;
    std::vector<ChromosomeOffset> chromosome_offsets_;

    // GPU resources (owned)
    std::unique_ptr<cuda::MemoryManager> gpu_mem_manager_;
    std::unique_ptr<GpuContextManager> gpu_context_manager_;
    bool gpu_enabled_;

    // Device memory
    cuda::ReadBatch d_read_batch_;
    cuda::Seed* d_seeds_;
    cuda::AlignmentResult* d_results_;
    cuda::FMIndex d_fm_index_;
    char* d_reference_;
    size_t reference_length_;
    cudaStream_t compute_stream_;

    // Pinned host memory
    char* pinned_read_sequences_;
    uint32_t* pinned_read_offsets_;
    uint32_t* pinned_read_lengths_;
    cuda::AlignmentResult* pinned_results_;
    size_t pinned_sequences_capacity_;
    size_t pinned_reads_capacity_;
    size_t pinned_results_capacity_;

    // BAM writer (owned)
    std::unique_ptr<BamWriter> bam_writer_;
};

} // namespace internal
} // namespace winalign
