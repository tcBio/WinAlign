#ifndef WINALIGN_AMPLICON_PIPELINE_EXECUTOR_H
#define WINALIGN_AMPLICON_PIPELINE_EXECUTOR_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <memory>
#include <vector>
#include <functional>
#include <thread>
#include <queue>

namespace winalign {
namespace amplicon {

/**
 * Pipeline stage for overlapping CPU/GPU work
 */
enum class PipelineStage {
    CPU_PREPROCESSING,   // Read collapsing, demux, trimming
    GPU_TRANSFER_H2D,    // Host to device memory transfer
    GPU_PROCESSING,      // GPU kernels (alignment, variant calling)
    GPU_TRANSFER_D2H,    // Device to host memory transfer
    CPU_POSTPROCESSING   // VCF writing, statistics
};

/**
 * Work batch for pipelined execution
 */
struct WorkBatch {
    uint32_t batch_id;
    std::vector<ReadCluster> clusters;
    std::vector<AmpliconAlignment> alignments;
    std::vector<AmpliconVariant> variants;
    PipelineStage current_stage;
    void* gpu_data;  // GPU-side data pointer

    WorkBatch() : batch_id(0), current_stage(PipelineStage::CPU_PREPROCESSING),
                  gpu_data(nullptr) {}
};

/**
 * Pipeline executor with overlapping stages
 *
 * Overlaps CPU preprocessing of batch N+1 with GPU processing of batch N
 * Significantly improves throughput by hiding latency.
 */
class PipelineExecutor {
public:
    PipelineExecutor(size_t num_pipeline_stages = 3);
    ~PipelineExecutor();

    /**
     * Configure pipeline
     */
    void set_batch_size(size_t batch_size);
    void set_num_cpu_threads(size_t num_threads);
    void set_gpu_device(int device_id);

    /**
     * Start pipeline execution
     */
    void start();

    /**
     * Stop pipeline
     */
    void stop();

    /**
     * Submit work to pipeline
     */
    void submit_batch(WorkBatch&& batch);

    /**
     * Wait for all batches to complete
     */
    void wait_for_completion();

    /**
     * Set stage callbacks
     */
    using StageCallback = std::function<void(WorkBatch&)>;

    void set_cpu_preprocessing_callback(StageCallback callback);
    void set_gpu_processing_callback(StageCallback callback);
    void set_cpu_postprocessing_callback(StageCallback callback);

    /**
     * Get pipeline statistics
     */
    struct Stats {
        uint64_t batches_processed;
        float avg_cpu_prep_time_ms;
        float avg_gpu_transfer_time_ms;
        float avg_gpu_compute_time_ms;
        float avg_cpu_post_time_ms;
        float pipeline_throughput;  // batches/sec
        float cpu_utilization;
        float gpu_utilization;
    };

    Stats get_stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Zero-copy memory manager for GPU transfers
 * Uses pinned host memory for faster transfers
 */
class ZeroCopyMemory {
public:
    ZeroCopyMemory();
    ~ZeroCopyMemory();

    /**
     * Allocate pinned host memory
     */
    void* allocate_pinned(size_t size);

    /**
     * Free pinned memory
     */
    void free_pinned(void* ptr);

    /**
     * Copy to GPU with zero-copy (if supported)
     */
    void* copy_to_device(const void* host_ptr, size_t size);

    /**
     * Copy from GPU
     */
    void copy_from_device(void* host_ptr, const void* device_ptr, size_t size);

    /**
     * Check if zero-copy is supported
     */
    bool is_zero_copy_supported() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_PIPELINE_EXECUTOR_H
