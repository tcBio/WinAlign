#ifndef WINALIGN_CUDA_ALIGNMENT_CUH
#define WINALIGN_CUDA_ALIGNMENT_CUH

#include "seeding.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

/**
 * @brief GPU alignment result
 */
struct AlignmentResult {
    uint32_t read_id;           // Read ID
    uint32_t read_length;       // Length of the read
    uint64_t position;          // Alignment start position
    int32_t score;              // Alignment score
    uint16_t cigar_length;      // CIGAR string length
    uint16_t flag;              // SAM flags
    uint8_t mapping_quality;    // Mapping quality
    uint8_t reserved;           // Padding/reserved
    uint16_t reserved2;         // Padding/reserved
    char* cigar;                // CIGAR string (device pointer)

    __host__ __device__
    AlignmentResult()
        : read_id(0),
          read_length(0),
          position(0),
          score(0),
          cigar_length(0),
          flag(0),
          mapping_quality(0),
          reserved(0),
          reserved2(0),
          cigar(nullptr) {}
};

/**
 * @brief Smith-Waterman alignment parameters
 */
struct SWParams {
    int32_t match_score = 1;
    int32_t mismatch_score = -4;
    int32_t gap_open = -6;
    int32_t gap_extend = -1;
};

/**
 * @brief Perform warp-optimized banded Smith-Waterman alignment on GPU (Phase 3)
 *
 * Uses one warp (32 threads) per alignment with banded DP for 3-5x speedup.
 * Band follows the diagonal within ±band_width cells.
 *
 * @param reads Input read batch on device
 * @param seeds Input seeds on device
 * @param num_seeds Number of seeds
 * @param reference Reference sequence on device
 * @param ref_length Reference length
 * @param params Alignment parameters
 * @param results Output alignment results on device
 * @param band_width Half-width of the alignment band (e.g., 64 = ±64 diagonal)
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t smith_waterman_align_banded_warp(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    const SWParams& params,
    AlignmentResult* results,
    uint32_t band_width,
    cudaStream_t stream = 0
);

/**
 * @brief Perform Smith-Waterman alignment on GPU
 *
 * Automatically selects the best kernel:
 * - Uses warp-optimized banded kernel (Phase 3) by default
 * - Falls back to original kernel for compatibility
 *
 * @param reads Input read batch on device
 * @param seeds Input seeds on device
 * @param num_seeds Number of seeds
 * @param reference Reference sequence on device
 * @param ref_length Reference length
 * @param params Alignment parameters
 * @param results Output alignment results on device
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t smith_waterman_align(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    const SWParams& params,
    AlignmentResult* results,
    cudaStream_t stream = 0
);

/**
 * @brief Batch Smith-Waterman alignment
 *
 * Process multiple reads in parallel with optimized batching.
 *
 * @param reads Input read batch on device
 * @param seeds Input seeds on device
 * @param num_seeds Number of seeds
 * @param reference Reference sequence on device
 * @param ref_length Reference length
 * @param params Alignment parameters
 * @param results Output alignment results on device
 * @param max_results Maximum number of results
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t batch_smith_waterman(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    const SWParams& params,
    AlignmentResult* results,
    uint32_t max_results,
    cudaStream_t stream = 0
);

/**
 * @brief Generate CIGAR string from alignment
 *
 * @param traceback Traceback matrix from alignment
 * @param read_length Read length
 * @param ref_length Reference length
 * @param cigar Output CIGAR string (device pointer)
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t generate_cigar(
    const uint8_t* traceback,
    uint32_t read_length,
    uint32_t ref_length,
    char* cigar,
    cudaStream_t stream = 0
);

/**
 * @brief Calculate mapping quality
 *
 * @param results Alignment results on device
 * @param num_results Number of results
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t calculate_mapping_quality(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream = 0
);

/**
 * @brief Allocate alignment results on device
 *
 * @param results Output alignment results array
 * @param max_results Maximum number of results
 * @param max_cigar_length Maximum CIGAR string length
 * @return cudaError_t CUDA error code
 */
cudaError_t allocate_alignment_results(
    AlignmentResult*& results,
    uint32_t max_results,
    uint32_t max_cigar_length
);

/**
 * @brief Free alignment results on device
 *
 * @param results Alignment results to free
 * @param num_results Number of results
 * @return cudaError_t CUDA error code
 */
cudaError_t free_alignment_results(
    AlignmentResult* results,
    uint32_t num_results
);

} // namespace cuda
} // namespace winalign

#endif // WINALIGN_CUDA_ALIGNMENT_CUH
