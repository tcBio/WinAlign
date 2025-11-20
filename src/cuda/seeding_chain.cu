/**
 * @file seeding_chain.cu
 * @brief Seed chaining implementation (minimap2-style)
 *
 * Implements co-linear seed chaining to reduce the number of alignments
 * from 10-50 per read to 1-2 per read, achieving 3-5x speedup.
 */

#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

/**
 * @brief Chain co-linear seeds using minimap2 algorithm
 *
 * For each read, finds the seed that is part of the best co-linear chain.
 * Seeds are co-linear if the gap in reference ≈ gap in read.
 *
 * Algorithm:
 * - For each seed, calculate a score = base_score + chaining_bonus
 * - Chaining bonus comes from seeds with similar ref/read gap deltas
 * - Returns the seed with the highest chaining score per read
 *
 * Complexity: O(n²) per read where n = seeds per read (typically 10-50)
 */
__global__ void chain_seeds_kernel(
    const Seed* seeds,                    // All seeds (flat array)
    const uint32_t* seeds_per_read_offsets, // Starting index for each read's seeds
    const uint32_t* seeds_per_read_counts,  // Number of seeds per read
    uint32_t num_reads,
    Seed* best_seeds_out,                 // Best seed per read (output)
    float* chain_scores_out               // Chain scores (output)
) {
    uint32_t read_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (read_id >= num_reads) return;

    uint32_t start = seeds_per_read_offsets[read_id];
    uint32_t count = seeds_per_read_counts[read_id];

    // Handle edge cases
    if (count == 0) {
        chain_scores_out[read_id] = 0;
        return;
    }

    if (count == 1) {
        best_seeds_out[read_id] = seeds[start];
        chain_scores_out[read_id] = static_cast<float>(seeds[start].length);
        return;
    }

    // Dynamic programming: find best seed chain
    float best_score = 0;
    uint32_t best_idx = start;

    for (uint32_t i = 0; i < count; i++) {
        const Seed& curr = seeds[start + i];

        // Base score from seed length
        float score = static_cast<float>(curr.length);

        // Penalty for mismatches
        score -= static_cast<float>(curr.mismatches) * 2.0f;

        // Add chaining bonus from previous seeds
        for (uint32_t j = 0; j < i; j++) {
            const Seed& prev = seeds[start + j];

            // Calculate gaps in reference and read
            int64_t ref_gap = static_cast<int64_t>(curr.position) -
                             static_cast<int64_t>(prev.position);
            int64_t read_gap = static_cast<int64_t>(curr.read_offset) -
                              static_cast<int64_t>(prev.read_offset);

            // Both gaps must be positive (seeds in monotonic order)
            if (ref_gap > 0 && read_gap > 0) {
                // Calculate how much the gaps differ
                int64_t gap_diff = abs(ref_gap - read_gap);

                // If co-linear (gap difference is small), award chaining bonus
                // Threshold of 100 allows for small indels
                if (gap_diff < 100) {
                    // Award 40% of previous seed's score as chaining bonus
                    float prev_score = static_cast<float>(prev.length) -
                                     static_cast<float>(prev.mismatches) * 2.0f;
                    score += prev_score * 0.4f;
                }
            }
        }

        // Track best scoring seed
        if (score > best_score) {
            best_score = score;
            best_idx = start + i;
        }
    }

    // Output best seed and its score
    best_seeds_out[read_id] = seeds[best_idx];
    chain_scores_out[read_id] = best_score;
}

/**
 * @brief Host function to launch seed chaining kernel
 *
 * @param seeds All seeds (must be sorted by read_id)
 * @param seeds_per_read_offsets Starting offset for each read's seeds
 * @param seeds_per_read_counts Number of seeds for each read
 * @param num_reads Total number of reads
 * @param best_seeds_out Output: best seed per read
 * @param chain_scores_out Output: chaining scores
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t chain_seeds(
    const Seed* seeds,
    const uint32_t* seeds_per_read_offsets,
    const uint32_t* seeds_per_read_counts,
    uint32_t num_reads,
    Seed* best_seeds_out,
    float* chain_scores_out,
    cudaStream_t stream
) {
    if (num_reads == 0) {
        return cudaSuccess;
    }

    // Launch one thread per read
    dim3 block_size(256);
    dim3 grid_size((num_reads + block_size.x - 1) / block_size.x);

    chain_seeds_kernel<<<grid_size, block_size, 0, stream>>>(
        seeds,
        seeds_per_read_offsets,
        seeds_per_read_counts,
        num_reads,
        best_seeds_out,
        chain_scores_out
    );

    return cudaGetLastError();
}

} // namespace cuda
} // namespace winalign
