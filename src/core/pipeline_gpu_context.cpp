/**
 * @file pipeline_gpu_context.cpp
 * @brief GPU context lifecycle management for multi-stream pipeline
 *
 * Manages allocation, initialization, and cleanup of GPU batch contexts
 * for the multi-stream async pipeline (Phase 1).
 */

#include "pipeline_gpu_context.h"
#include "winalign/logger.h"
#include "winalign/cuda/memory_manager.cuh"
#include "winalign/cuda/seeding.cuh"
#include "winalign/cuda/alignment.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace internal {

// Helper constant (also in pipeline_internal.h, but repeated for clarity)
constexpr uint32_t MAX_CIGAR_LENGTH = 256;

GpuContextManager::GpuContextManager(
    const PipelineConfig& config,
    cuda::MemoryManager* mem_manager,
    bool gpu_enabled)
    : config_(config)
    , mem_manager_(mem_manager)
    , gpu_enabled_(gpu_enabled)
    , initialized_(false)
{
}

GpuContextManager::~GpuContextManager() {
    cleanup();
}

bool GpuContextManager::initialize() {
    if (!gpu_enabled_ || !mem_manager_) {
        Logger::instance().warn("GPU not enabled or memory manager not available");
        return false;
    }

    if (initialized_) {
        Logger::instance().warn("GPU contexts already initialized");
        return true;
    }

    contexts_.resize(NUM_GPU_CONTEXTS);

    bool is_paired = !config_.read2_fastq.empty();
    uint32_t max_reads_per_batch = static_cast<uint32_t>(
        config_.batch_size * (is_paired ? 2u : 1u));
    uint32_t max_seeds = max_reads_per_batch *
        (MAX_READ_LENGTH - static_cast<uint32_t>(config_.kmer_size) + 1);

    Logger::instance().info(
        "Initializing " + std::to_string(NUM_GPU_CONTEXTS) + " GPU contexts for multi-stream pipeline");
    Logger::instance().info(
        "  Max reads per batch: " + std::to_string(max_reads_per_batch));
    Logger::instance().info(
        "  Max seeds: " + std::to_string(max_seeds));

    for (int i = 0; i < NUM_GPU_CONTEXTS; ++i) {
        auto& ctx = contexts_[i];
        ctx.context_id = i;
        ctx.state = ContextState::EMPTY;

        // Create dedicated stream for this context
        cudaError_t err = cudaStreamCreate(&ctx.stream);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to create stream for context " + std::to_string(i) +
                ": " + cudaGetErrorString(err));
            cleanup();
            return false;
        }

        // Create events for this context
        if (cudaEventCreate(&ctx.event_h2d_done) != cudaSuccess ||
            cudaEventCreate(&ctx.event_seeding_done) != cudaSuccess ||
            cudaEventCreate(&ctx.event_sw_done) != cudaSuccess ||
            cudaEventCreate(&ctx.event_d2h_done) != cudaSuccess) {
            Logger::instance().error("Failed to create events for context " + std::to_string(i));
            cleanup();
            return false;
        }

        // Allocate device buffers for this context
        err = cuda::allocate_read_batch(ctx.d_read_batch, max_reads_per_batch, MAX_READ_LENGTH);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate read batch for context " + std::to_string(i));
            cleanup();
            return false;
        }

        err = mem_manager_->allocate(
            max_seeds * sizeof(cuda::Seed),
            (void**)&ctx.d_seeds);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate seeds for context " + std::to_string(i));
            cleanup();
            return false;
        }

        err = cuda::allocate_alignment_results(
            ctx.d_results, max_seeds, MAX_CIGAR_LENGTH);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate results for context " + std::to_string(i));
            cleanup();
            return false;
        }

        // Allocate seed chaining buffers (for 3-5x speedup)
        err = mem_manager_->allocate(
            max_reads_per_batch * sizeof(cuda::Seed),
            (void**)&ctx.d_best_seeds);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate best_seeds for context " + std::to_string(i));
            cleanup();
            return false;
        }

        err = mem_manager_->allocate(
            max_reads_per_batch * sizeof(float),
            (void**)&ctx.d_chain_scores);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate chain_scores for context " + std::to_string(i));
            cleanup();
            return false;
        }

        err = mem_manager_->allocate(
            max_reads_per_batch * sizeof(uint32_t),
            (void**)&ctx.d_seeds_per_read_offsets);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate seed offsets for context " + std::to_string(i));
            cleanup();
            return false;
        }

        err = mem_manager_->allocate(
            max_reads_per_batch * sizeof(uint32_t),
            (void**)&ctx.d_seeds_per_read_counts);
        if (err != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate seed counts for context " + std::to_string(i));
            cleanup();
            return false;
        }

        // Allocate pinned host buffers for async transfer
        size_t seq_capacity = static_cast<size_t>(max_reads_per_batch) * MAX_READ_LENGTH;

        if (mem_manager_->allocate_pinned(seq_capacity, (void**)&ctx.pinned_sequences) != cudaSuccess ||
            mem_manager_->allocate_pinned(max_reads_per_batch * sizeof(uint32_t), (void**)&ctx.pinned_offsets) != cudaSuccess ||
            mem_manager_->allocate_pinned(max_reads_per_batch * sizeof(uint32_t), (void**)&ctx.pinned_lengths) != cudaSuccess ||
            mem_manager_->allocate_pinned(max_seeds * sizeof(cuda::Seed), (void**)&ctx.pinned_seeds) != cudaSuccess ||
            mem_manager_->allocate_pinned(max_seeds * sizeof(cuda::AlignmentResult), (void**)&ctx.pinned_results) != cudaSuccess) {
            Logger::instance().error(
                "Failed to allocate pinned memory for context " + std::to_string(i));
            cleanup();
            return false;
        }

        Logger::instance().info("Initialized GPU context " + std::to_string(i));
    }

    initialized_ = true;
    Logger::instance().info("Multi-stream GPU scheduler initialized with " +
                           std::to_string(NUM_GPU_CONTEXTS) + " contexts");
    return true;
}

void GpuContextManager::cleanup() {
    if (!initialized_ && contexts_.empty()) {
        return;
    }

    Logger::instance().info("Cleaning up GPU contexts");

    for (auto& ctx : contexts_) {
        // Synchronize and destroy stream
        if (ctx.stream) {
            cudaStreamSynchronize(ctx.stream);
            cudaStreamDestroy(ctx.stream);
            ctx.stream = nullptr;
        }

        // Destroy events
        if (ctx.event_h2d_done) {
            cudaEventDestroy(ctx.event_h2d_done);
            ctx.event_h2d_done = nullptr;
        }
        if (ctx.event_seeding_done) {
            cudaEventDestroy(ctx.event_seeding_done);
            ctx.event_seeding_done = nullptr;
        }
        if (ctx.event_sw_done) {
            cudaEventDestroy(ctx.event_sw_done);
            ctx.event_sw_done = nullptr;
        }
        if (ctx.event_d2h_done) {
            cudaEventDestroy(ctx.event_d2h_done);
            ctx.event_d2h_done = nullptr;
        }

        // Free device memory
        if (ctx.d_seeds && mem_manager_) {
            mem_manager_->free(ctx.d_seeds);
            ctx.d_seeds = nullptr;
        }
        if (ctx.d_results) {
            cuda::free_alignment_results(ctx.d_results, ctx.num_seeds);
            ctx.d_results = nullptr;
        }
        cuda::free_read_batch(ctx.d_read_batch);

        // Free seed chaining buffers
        if (ctx.d_best_seeds && mem_manager_) {
            mem_manager_->free(ctx.d_best_seeds);
            ctx.d_best_seeds = nullptr;
        }
        if (ctx.d_chain_scores && mem_manager_) {
            mem_manager_->free(ctx.d_chain_scores);
            ctx.d_chain_scores = nullptr;
        }
        if (ctx.d_seeds_per_read_offsets && mem_manager_) {
            mem_manager_->free(ctx.d_seeds_per_read_offsets);
            ctx.d_seeds_per_read_offsets = nullptr;
        }
        if (ctx.d_seeds_per_read_counts && mem_manager_) {
            mem_manager_->free(ctx.d_seeds_per_read_counts);
            ctx.d_seeds_per_read_counts = nullptr;
        }

        // Free pinned host memory
        if (mem_manager_) {
            if (ctx.pinned_sequences) {
                mem_manager_->free_pinned(ctx.pinned_sequences);
                ctx.pinned_sequences = nullptr;
            }
            if (ctx.pinned_offsets) {
                mem_manager_->free_pinned(ctx.pinned_offsets);
                ctx.pinned_offsets = nullptr;
            }
            if (ctx.pinned_lengths) {
                mem_manager_->free_pinned(ctx.pinned_lengths);
                ctx.pinned_lengths = nullptr;
            }
            if (ctx.pinned_seeds) {
                mem_manager_->free_pinned(ctx.pinned_seeds);
                ctx.pinned_seeds = nullptr;
            }
            if (ctx.pinned_results) {
                mem_manager_->free_pinned(ctx.pinned_results);
                ctx.pinned_results = nullptr;
            }
        }
    }

    contexts_.clear();
    initialized_ = false;
    Logger::instance().info("GPU contexts cleaned up");
}

std::vector<GpuBatchContext>& GpuContextManager::get_contexts() {
    return contexts_;
}

const std::vector<GpuBatchContext>& GpuContextManager::get_contexts() const {
    return contexts_;
}

bool GpuContextManager::is_initialized() const {
    return initialized_;
}

size_t GpuContextManager::num_contexts() const {
    return contexts_.size();
}

} // namespace internal
} // namespace winalign
