/**
 * @file pipeline_multistream.h
 * @brief Multi-stream GPU scheduler for overlapped pipeline processing
 */

#pragma once

#include "pipeline_internal.h"
#include "winalign/common.h"
#include "winalign/result.h"
#include "winalign/cuda/seeding.cuh"
#include "winalign/cuda/alignment.cuh"
#include <vector>
#include <atomic>
#include <functional>

namespace winalign {

// Forward declarations
struct PipelineConfig;
class BamWriter;

namespace cuda {
struct FMIndexView;
}

namespace internal {

// Forward declaration
class MetricsCollector;

/**
 * @brief Multi-stream GPU scheduler with async pipeline stages
 *
 * Implements Phase 1 multi-stream scheduler that overlaps FASTQ reading,
 * GPU transfers, GPU kernels, and BAM writing across multiple contexts.
 */
class MultiStreamScheduler {
public:
    /**
     * @brief Construct multi-stream scheduler
     * @param config Pipeline configuration
     * @param contexts GPU batch contexts (managed externally)
     * @param metrics Metrics collector (managed externally)
     * @param cancelled Cancellation flag
     */
    MultiStreamScheduler(
        const PipelineConfig& config,
        std::vector<GpuBatchContext>& contexts,
        MetricsCollector& metrics,
        std::atomic<bool>& cancelled);

    /**
     * @brief Initialize scheduler resources
     * @param d_fm_index Device FM index pointer
     * @param d_reference Device reference sequence pointer
     * @param reference_length Length of reference sequence
     * @param bam_writer BAM writer for output
     * @param estimate_total_reads Estimated total reads for progress
     * @param update_progress_fn Progress update callback
     * @return true if initialization succeeded
     */
    bool initialize(
        const cuda::FMIndexView* d_fm_index,
        const char* d_reference,
        size_t reference_length,
        BamWriter* bam_writer,
        uint64_t estimate_total_reads,
        std::function<void(double, const std::string&)> update_progress_fn);

    /**
     * @brief Run the multi-stream scheduler
     * @return Result indicating success or failure
     */
    Result<bool> run();

private:
    // === Helper Functions ===

    /**
     * @brief Load FASTQ batch from worker into context
     */
    void load_fastq_into_context_from_worker(
        GpuBatchContext& ctx,
        std::vector<ReadPair>&& pairs,
        std::vector<Read>&& singles,
        size_t count,
        bool is_paired,
        bool eof);

    /**
     * @brief Prepare context for GPU by flattening reads
     */
    bool prepare_context_for_gpu(GpuBatchContext& ctx);

    /**
     * @brief Launch async H2D transfer
     */
    cudaError_t launch_h2d_transfer(GpuBatchContext& ctx);

    /**
     * @brief Launch GPU seeding kernel
     */
    cudaError_t launch_seeding(GpuBatchContext& ctx);

    /**
     * @brief Launch GPU alignment kernel
     */
    cudaError_t launch_alignment(GpuBatchContext& ctx);

    /**
     * @brief Launch async D2H transfer
     */
    cudaError_t launch_d2h_transfer(GpuBatchContext& ctx);

    /**
     * @brief Process completed context results
     */
    void process_context_results(GpuBatchContext& ctx);

    // Configuration and state
    const PipelineConfig& config_;
    std::vector<GpuBatchContext>& gpu_contexts_;
    MetricsCollector& metrics_;
    std::atomic<bool>& cancelled_;

    // GPU resources (not owned)
    const cuda::FMIndexView* d_fm_index_;
    const char* d_reference_;
    size_t reference_length_;
    BamWriter* bam_writer_;

    // Progress tracking
    uint64_t estimated_total_reads_;
    std::function<void(double, const std::string&)> update_progress_fn_;
};

} // namespace internal
} // namespace winalign
