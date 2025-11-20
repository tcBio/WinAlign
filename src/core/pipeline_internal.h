#pragma once

#include "winalign/pipeline.h"
#include "winalign/common.h"
#include "winalign/cuda/seeding.cuh"
#include "winalign/cuda/alignment.cuh"
#include <vector>
#include <chrono>
#include <cuda_runtime.h>

namespace winalign {
namespace internal {

// === Phase 0: Profiling Structures ===

// Per-batch timing (in milliseconds)
struct BatchTiming {
    double t_read = 0.0;        // FASTQ read time
    double t_h2d = 0.0;         // Host-to-device transfer
    double t_gpu_seed = 0.0;    // GPU seeding kernel time
    double t_gpu_align = 0.0;   // GPU alignment kernel time
    double t_d2h = 0.0;         // Device-to-host transfer
    double t_write = 0.0;       // BAM write time
    double t_total = 0.0;       // Total batch time
};

// Performance counters
struct PerformanceCounters {
    uint64_t gpu_aligned_reads = 0;
    uint64_t unmapped_reads = 0;
    uint64_t reads_without_seeds = 0;
    uint64_t total_gpu_seeds = 0;
    uint64_t batches_processed = 0;

    double avg_seeds_per_read() const {
        return gpu_aligned_reads > 0
            ? static_cast<double>(total_gpu_seeds) / gpu_aligned_reads
            : 0.0;
    }
};

// === Phase 1: Multi-stream GPU Context ===

// GPU batch context state machine
enum class ContextState {
    EMPTY,          // Ready to be filled
    LOADING,        // Reading FASTQ data
    READY,          // H2D transfer ready
    TRANSFERRING,   // H2D transfer in progress
    SEEDING,        // GPU seeding in progress
    ALIGNING,       // GPU alignment in progress
    COPYING_BACK,   // D2H transfer in progress
    DONE,           // Ready for host processing
    PROCESSING      // Host-side processing (BAM write)
};

// Read view for GPU pipeline
struct ReadView {
    const Read* read = nullptr;
    bool is_paired = false;
    bool is_second = false;
};

// GPU batch context - one per concurrent stream
struct GpuBatchContext {
    // State
    ContextState state = ContextState::EMPTY;
    uint32_t context_id = 0;

    // Host data
    std::vector<ReadPair> host_pairs;
    std::vector<Read> host_singles;
    size_t read_count = 0;
    bool is_paired = false;
    bool eof = false;

    // Flattened host buffers (for GPU transfer)
    std::vector<char> host_read_sequences;
    std::vector<uint32_t> host_read_offsets;
    std::vector<uint32_t> host_read_lengths;
    std::vector<ReadView> read_views;

    // Pinned host buffers (for async transfer)
    char* pinned_sequences = nullptr;
    uint32_t* pinned_offsets = nullptr;
    uint32_t* pinned_lengths = nullptr;
    cuda::Seed* pinned_seeds = nullptr;
    cuda::AlignmentResult* pinned_results = nullptr;

    // Device buffers
    cuda::ReadBatch d_read_batch;
    cuda::Seed* d_seeds = nullptr;
    cuda::AlignmentResult* d_results = nullptr;

    // Seed and result data
    std::vector<cuda::Seed> host_seeds;
    std::vector<cuda::AlignmentResult> host_results;
    uint32_t num_seeds = 0;

    // Seed chaining (NEW - for 3-5x speedup)
    cuda::Seed* d_best_seeds = nullptr;       // One best seed per read
    float* d_chain_scores = nullptr;          // Chain scores per read
    uint32_t* d_seeds_per_read_offsets = nullptr; // Offset to each read's seeds
    uint32_t* d_seeds_per_read_counts = nullptr;  // Count of seeds per read

    // CUDA stream and events
    cudaStream_t stream = nullptr;
    cudaEvent_t event_h2d_done = nullptr;
    cudaEvent_t event_seeding_done = nullptr;
    cudaEvent_t event_sw_done = nullptr;
    cudaEvent_t event_d2h_done = nullptr;

    // Timing for this batch
    BatchTiming timing;
    std::chrono::high_resolution_clock::time_point batch_start;
};

// Constants
constexpr int NUM_GPU_CONTEXTS = 3;  // Overlap 3 batches for multi-stream
constexpr uint32_t MAX_SEEDS_PER_READ = 64;
constexpr uint32_t MAX_HITS_PER_SEED = 8;
constexpr uint32_t WINDOW_FLANK = 64;
constexpr uint32_t MAX_READ_LENGTH = 512;

} // namespace internal
} // namespace winalign
