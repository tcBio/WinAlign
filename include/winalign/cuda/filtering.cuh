#ifndef WINALIGN_CUDA_FILTERING_CUH
#define WINALIGN_CUDA_FILTERING_CUH

#include "alignment.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

/**
 * @brief Filtering parameters
 */
struct FilterParams {
    uint8_t min_mapping_quality = 0;
    uint32_t min_alignment_score = 0;
    uint32_t max_edit_distance = UINT32_MAX;
    bool filter_secondary = false;
    bool filter_supplementary = false;
};

/**
 * @brief Filter alignments by quality thresholds
 *
 * @param results Input/output alignment results on device
 * @param num_results Number of results
 * @param params Filter parameters
 * @param stream CUDA stream for async execution
 * @return Number of alignments passing filter
 */
uint32_t filter_by_quality(
    AlignmentResult* results,
    uint32_t num_results,
    const FilterParams& params,
    cudaStream_t stream = 0
);

/**
 * @brief Mark duplicate alignments
 *
 * Identifies and marks PCR/optical duplicates based on alignment positions.
 *
 * @param results Input/output alignment results on device
 * @param num_results Number of results
 * @param stream CUDA stream for async execution
 * @return Number of duplicates marked
 */
uint32_t mark_duplicates(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream = 0
);

/**
 * @brief Validate paired-end alignments
 *
 * @param results1 Forward read alignments on device
 * @param results2 Reverse read alignments on device
 * @param num_pairs Number of read pairs
 * @param min_insert_size Minimum insert size
 * @param max_insert_size Maximum insert size
 * @param stream CUDA stream for async execution
 * @return Number of proper pairs
 */
uint32_t validate_pairs(
    AlignmentResult* results1,
    AlignmentResult* results2,
    uint32_t num_pairs,
    uint32_t min_insert_size,
    uint32_t max_insert_size,
    cudaStream_t stream = 0
);

/**
 * @brief Sort alignments by coordinate
 *
 * Uses GPU-accelerated sorting (thrust::sort or similar).
 *
 * @param results Input/output alignment results on device
 * @param num_results Number of results
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t sort_by_coordinate(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream = 0
);

/**
 * @brief Compact alignment results
 *
 * Remove filtered-out alignments and compact the array.
 *
 * @param results Input/output alignment results on device
 * @param num_results Number of results
 * @param stream CUDA stream for async execution
 * @return New number of results after compaction
 */
uint32_t compact_results(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream = 0
);

/**
 * @brief Calculate alignment statistics
 */
struct AlignmentStats {
    uint32_t total_alignments;
    uint32_t primary_alignments;
    uint32_t secondary_alignments;
    uint32_t duplicates;
    float mean_mapping_quality;
    float mean_alignment_score;
};

/**
 * @brief Compute alignment statistics on GPU
 *
 * @param results Alignment results on device
 * @param num_results Number of results
 * @param stats Output statistics (host pointer)
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t compute_statistics(
    const AlignmentResult* results,
    uint32_t num_results,
    AlignmentStats* stats,
    cudaStream_t stream = 0
);

} // namespace cuda
} // namespace winalign

#endif // WINALIGN_CUDA_FILTERING_CUH
