#include "winalign/cuda/alignment.cuh"
#include <cuda_runtime.h>
#include <cub/cub.cuh>

namespace winalign {
namespace cuda {

// Device function: Match/mismatch score
__device__ inline int32_t match_score(char a, char b, const SWParams& params) {
    if (a == b || a == 'N' || b == 'N') {
        return params.match_score;
    }
    return params.mismatch_score;
}

// Device function: Maximum of three values
__device__ inline int32_t max3(int32_t a, int32_t b, int32_t c) {
    return max(max(a, b), c);
}

// Smith-Waterman alignment kernel - one thread per alignment
__global__ void smith_waterman_kernel(
    const char* reads,
    const uint32_t* read_offsets,
    const uint32_t* read_lengths,
    const Seed* seeds,
    const char* reference,
    uint64_t ref_length,
    SWParams params,
    AlignmentResult* results
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;

    // Each thread handles one seed
    const Seed& seed = seeds[tid];
    uint32_t read_id = seed.read_id;
    uint32_t read_offset = read_offsets[read_id];
    uint32_t read_len = read_lengths[read_id];

    const char* read = reads + read_offset;
    uint64_t ref_pos = seed.position;

    // Clamp reference window
    if (ref_pos >= ref_length) {
        results[tid].score = 0;
        results[tid].position = 0;
        return;
    }

    // Extension window around seed
    const uint32_t WINDOW = 100;
    uint64_t ref_start = (ref_pos > WINDOW) ? (ref_pos - WINDOW) : 0;
    uint64_t ref_end = min(ref_pos + read_len + WINDOW, ref_length);
    uint32_t ref_win_len = ref_end - ref_start;

    const char* ref_win = reference + ref_start;

    // Allocate DP matrix in shared memory (small window only)
    // For larger alignments, would need different strategy
    const uint32_t MAX_DP_SIZE = 256;

    if (read_len > MAX_DP_SIZE || ref_win_len > MAX_DP_SIZE) {
        // Fallback: simple scoring without full DP
        int32_t score = 0;
        uint32_t matches = 0;
        uint32_t len = min(read_len, ref_win_len);

        for (uint32_t i = 0; i < len; ++i) {
            if (read[i] == ref_win[i]) {
                score += params.match_score;
                matches++;
            } else {
                score += params.mismatch_score;
            }
        }

        results[tid].read_id = read_id;
        results[tid].position = ref_start;
        results[tid].score = score;
        results[tid].cigar_length = 0;
        results[tid].flag = 0;
        results[tid].mapping_quality = (matches * 60) / read_len; // Rough MAPQ

        return;
    }

    // Simple Smith-Waterman with linear gap penalty (simplified)
    int32_t best_score = 0;
    uint32_t best_i = 0, best_j = 0;

    // Initialize DP matrix (first row and column)
    int32_t H[MAX_DP_SIZE];

    for (uint32_t i = 0; i < MAX_DP_SIZE; ++i) {
        H[i] = 0;
    }

    // Fill DP matrix
    for (uint32_t i = 1; i <= read_len && i < MAX_DP_SIZE; ++i) {
        int32_t prev_diag = 0;
        int32_t prev_left = 0;

        for (uint32_t j = 1; j <= ref_win_len && j < MAX_DP_SIZE; ++j) {
            // Match/mismatch
            int32_t diag = prev_diag + match_score(read[i-1], ref_win[j-1], params);

            // Gap in read
            int32_t up = H[j] + params.gap_open;

            // Gap in reference
            int32_t left = prev_left + params.gap_open;

            // Take maximum, min 0 for local alignment
            int32_t score = max3(diag, up, left);
            score = max(score, 0);

            prev_diag = H[j];
            prev_left = score;
            H[j] = score;

            // Track best score
            if (score > best_score) {
                best_score = score;
                best_i = i;
                best_j = j;
            }
        }
    }

    // Store result
    results[tid].read_id = read_id;
    results[tid].position = ref_start + best_j;
    results[tid].score = best_score;
    results[tid].cigar_length = 0; // Would generate from traceback
    results[tid].flag = 0;
    results[tid].mapping_quality = (best_score > 0) ? min(60, best_score / 2) : 0;
}

// CIGAR operations
enum CigarOp { MATCH = 0, INSERT = 1, DELETE = 2 };

// Kernel: Generate CIGAR from traceback
__global__ void generate_cigar_kernel(
    const uint8_t* traceback,
    uint32_t read_length,
    uint32_t ref_length,
    char* cigar,
    uint32_t max_cigar_len
) {
    // Simplified: Generate CIGAR from traceback matrix
    // In production, would trace back through DP matrix

    uint32_t tid = threadIdx.x;
    if (tid != 0) return;

    // For now, generate simple all-match CIGAR
    cigar[0] = '0' + (read_length % 10);
    cigar[1] = 'M';
    cigar[2] = '\0';
}

// Kernel: Calculate mapping quality based on alignment score
__global__ void calculate_mapq_kernel(
    AlignmentResult* results,
    uint32_t num_results
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= num_results) return;

    AlignmentResult& result = results[tid];

    // Simple MAPQ calculation based on score
    // In production, would consider secondary alignments
    if (result.score <= 0) {
        result.mapping_quality = 0;
    } else if (result.score > 60) {
        result.mapping_quality = 60;
    } else {
        result.mapping_quality = static_cast<uint8_t>(result.score);
    }
}

// Host function: Launch Smith-Waterman alignment
cudaError_t smith_waterman_align(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    const SWParams& params,
    AlignmentResult* results,
    cudaStream_t stream
) {
    if (num_seeds == 0) return cudaSuccess;

    dim3 block_size(256);
    dim3 grid_size((num_seeds + block_size.x - 1) / block_size.x);

    smith_waterman_kernel<<<grid_size, block_size, 0, stream>>>(
        reads.sequences,
        reads.offsets,
        reads.lengths,
        seeds,
        reference,
        ref_length,
        params,
        results
    );

    return cudaGetLastError();
}

// Host function: Batch Smith-Waterman with optimizations
cudaError_t batch_smith_waterman(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    const SWParams& params,
    AlignmentResult* results,
    uint32_t max_results,
    cudaStream_t stream
) {
    // Process in batches if needed
    uint32_t batch_size = min(num_seeds, max_results);

    return smith_waterman_align(
        reads, seeds, batch_size, reference, ref_length, params, results, stream
    );
}

// Host function: Generate CIGAR strings
cudaError_t generate_cigar(
    const uint8_t* traceback,
    uint32_t read_length,
    uint32_t ref_length,
    char* cigar,
    cudaStream_t stream
) {
    dim3 block_size(1);
    dim3 grid_size(1);

    generate_cigar_kernel<<<grid_size, block_size, 0, stream>>>(
        traceback, read_length, ref_length, cigar, 1024
    );

    return cudaGetLastError();
}

// Host function: Calculate mapping quality
cudaError_t calculate_mapping_quality(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream
) {
    if (num_results == 0) return cudaSuccess;

    dim3 block_size(256);
    dim3 grid_size((num_results + block_size.x - 1) / block_size.x);

    calculate_mapq_kernel<<<grid_size, block_size, 0, stream>>>(
        results, num_results
    );

    return cudaGetLastError();
}

cudaError_t allocate_alignment_results(
    AlignmentResult*& results,
    uint32_t max_results,
    uint32_t max_cigar_length
) {
    cudaError_t err = cudaMalloc(&results, max_results * sizeof(AlignmentResult));
    if (err != cudaSuccess) return err;

    // TODO: Allocate CIGAR strings
    return cudaSuccess;
}

cudaError_t free_alignment_results(
    AlignmentResult* results,
    uint32_t num_results
) {
    if (results) cudaFree(results);
    return cudaSuccess;
}

} // namespace cuda
} // namespace winalign
