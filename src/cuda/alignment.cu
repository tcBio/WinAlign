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

// ===== PHASE 3: Warp-Optimized Banded Smith-Waterman =====

// Warp-optimized banded SW kernel - one warp (32 threads) per alignment
// Uses banded DP for better performance and memory efficiency
__global__ void smith_waterman_banded_warp_kernel(
    const char* reads,
    const uint32_t* read_offsets,
    const uint32_t* read_lengths,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    SWParams params,
    AlignmentResult* results,
    uint32_t band_width  // Half-width of the band (e.g., 64 means ±64 diagonal)
) {
    // One warp per alignment
    const uint32_t WARP_SIZE = 32;
    uint32_t warp_id = (blockIdx.x * blockDim.x + threadIdx.x) / WARP_SIZE;
    uint32_t lane_id = threadIdx.x % WARP_SIZE;

    if (warp_id >= num_seeds) return;

    // Get seed and read info
    const Seed& seed = seeds[warp_id];
    uint32_t read_id = seed.read_id;
    uint32_t read_offset = read_offsets[read_id];
    uint32_t read_len = read_lengths[read_id];

    const char* read = reads + read_offset;
    uint64_t ref_pos = seed.position;

    // Bounds check
    if (ref_pos >= ref_length) {
        if (lane_id == 0) {
            results[warp_id].score = 0;
            results[warp_id].position = 0;
            results[warp_id].read_id = read_id;
            results[warp_id].read_length = read_len;
        }
        return;
    }

    // Extension window around seed
    const uint32_t FLANK = 100;
    uint64_t ref_start = (ref_pos > FLANK) ? (ref_pos - FLANK) : 0;
    uint64_t ref_end = min(ref_pos + read_len + FLANK, ref_length);
    uint32_t ref_win_len = ref_end - ref_start;
    const char* ref_win = reference + ref_start;

    // Shared memory for band DP (2 rows: current and previous)
    extern __shared__ int32_t shared_mem[];
    int32_t* H_curr = shared_mem + (threadIdx.x / WARP_SIZE) * (band_width * 2 + 1) * 2;
    int32_t* H_prev = H_curr + (band_width * 2 + 1);

    // Initialize band
    const uint32_t band_size = band_width * 2 + 1;
    const int32_t NEG_INF = -1000000000;

    for (uint32_t k = lane_id; k < band_size; k += WARP_SIZE) {
        H_curr[k] = 0;
        H_prev[k] = 0;
    }
    __syncwarp();

    int32_t best_score = 0;
    uint32_t best_i = 0, best_j = 0;

    // Banded DP: Process each row of the read
    for (uint32_t i = 1; i <= read_len && i <= 512; ++i) {
        char read_base = read[i - 1];

        // Swap buffers
        int32_t* tmp = H_prev;
        H_prev = H_curr;
        H_curr = tmp;

        // For row i, we process columns in the band: [i - band_width, i + band_width]
        // But clamp to [1, ref_win_len]
        int32_t j_min = max(1, (int32_t)i - (int32_t)band_width);
        int32_t j_max = min((int32_t)ref_win_len, (int32_t)i + (int32_t)band_width);

        // Each thread in warp handles multiple cells in the band
        for (int32_t j = j_min + lane_id; j <= j_max; j += WARP_SIZE) {
            if (j < 1 || j > (int32_t)ref_win_len) continue;

            char ref_base = ref_win[j - 1];

            // Band index for current position
            int32_t band_idx = j - (int32_t)i + (int32_t)band_width;
            if (band_idx < 0 || band_idx >= (int32_t)band_size) continue;

            // Get values from previous row/column using warp shuffle
            int32_t diag_val = 0;
            int32_t up_val = 0;
            int32_t left_val = 0;

            // Diagonal (i-1, j-1)
            int32_t prev_band_idx = band_idx; // Same band position in previous row
            if (prev_band_idx >= 0 && prev_band_idx < (int32_t)band_size) {
                diag_val = H_prev[prev_band_idx];
            }

            // Up (i-1, j)
            int32_t up_band_idx = band_idx + 1; // One position right in band
            if (up_band_idx >= 0 && up_band_idx < (int32_t)band_size) {
                up_val = H_prev[up_band_idx];
            } else {
                up_val = NEG_INF;
            }

            // Left (i, j-1) - need value from same row, previous column
            if (j > j_min) {
                // Use warp shuffle to get from adjacent thread
                left_val = __shfl_up_sync(0xFFFFFFFF, H_curr[band_idx], 1);
                if (lane_id == 0 || j == j_min) {
                    // First thread or leftmost in band - use value from band
                    int32_t left_band_idx = band_idx - 1;
                    if (left_band_idx >= 0 && left_band_idx < (int32_t)band_size) {
                        left_val = H_curr[left_band_idx];
                    } else {
                        left_val = 0;
                    }
                }
            } else {
                left_val = 0;
            }

            // Compute match/mismatch score
            int32_t match = diag_val + match_score(read_base, ref_base, params);

            // Gap penalties (simplified affine - can be enhanced)
            int32_t gap_up = up_val + params.gap_open;
            int32_t gap_left = left_val + params.gap_open;

            // SW local alignment: max(0, match, gaps)
            int32_t score = max3(0, match, max(gap_up, gap_left));

            // Store in current band
            H_curr[band_idx] = score;

            // Track best score (will reduce across warp later)
            if (score > best_score) {
                best_score = score;
                best_i = i;
                best_j = j;
            }
        }

        __syncwarp();
    }

    // Warp reduction to find global best score
    int32_t warp_best_score = best_score;
    uint32_t warp_best_i = best_i;
    uint32_t warp_best_j = best_j;

    for (int offset = WARP_SIZE / 2; offset > 0; offset /= 2) {
        int32_t other_score = __shfl_down_sync(0xFFFFFFFF, warp_best_score, offset);
        uint32_t other_i = __shfl_down_sync(0xFFFFFFFF, warp_best_i, offset);
        uint32_t other_j = __shfl_down_sync(0xFFFFFFFF, warp_best_j, offset);

        if (other_score > warp_best_score) {
            warp_best_score = other_score;
            warp_best_i = other_i;
            warp_best_j = other_j;
        }
    }

    // Lane 0 writes the result
    if (lane_id == 0) {
        uint64_t align_start = ref_start;
        if (warp_best_j > warp_best_i) {
            align_start = ref_start + (warp_best_j - warp_best_i);
        }
        if (align_start >= ref_length) {
            align_start = (ref_length > 0) ? (ref_length - 1) : 0;
        }

        results[warp_id].read_id = read_id;
        results[warp_id].read_length = read_len;
        results[warp_id].position = align_start;
        results[warp_id].score = warp_best_score;
        results[warp_id].cigar_length = 0;
        results[warp_id].flag = 0;
        results[warp_id].mapping_quality = (warp_best_score > 0) ? min(60, warp_best_score / 2) : 0;
    }
}

// ===== Original Smith-Waterman kernel (kept for fallback) =====

// Smith-Waterman alignment kernel - one thread per alignment
__global__ void smith_waterman_kernel(
    const char* reads,
    const uint32_t* read_offsets,
    const uint32_t* read_lengths,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    SWParams params,
    AlignmentResult* results
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;

    // Bounds check
    if (tid >= num_seeds) return;

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
        results[tid].read_length = read_len;
        results[tid].position = ref_start;
        results[tid].score = score;
        results[tid].cigar_length = 0;
        results[tid].flag = 0;
        results[tid].mapping_quality = (matches * 60) / read_len; // Rough MAPQ

        return;
    }

    // Smith-Waterman with affine gap penalty
    int32_t best_score = 0;
    uint32_t best_i = 0, best_j = 0;
    const int32_t NEG_INF = -1000000000;

    int32_t H[MAX_DP_SIZE];
    int32_t E[MAX_DP_SIZE];

    for (uint32_t i = 0; i < MAX_DP_SIZE; ++i) {
        H[i] = 0;
        E[i] = NEG_INF;
    }

    for (uint32_t i = 1; i <= read_len && i < MAX_DP_SIZE; ++i) {
        int32_t prev_diag = 0;
        int32_t prev_left = 0;
        int32_t F = NEG_INF;

        for (uint32_t j = 1; j <= ref_win_len && j < MAX_DP_SIZE; ++j) {
            // Match/mismatch
            int32_t diag = prev_diag + match_score(read[i-1], ref_win[j-1], params);

            // Gap in read (vertical)
            int32_t up = max(H[j] + params.gap_open,
                             E[j] + params.gap_extend);
            E[j] = up;

            // Gap in reference (horizontal)
            F = max(prev_left + params.gap_open,
                    F + params.gap_extend);

            // Take maximum, min 0 for local alignment
            int32_t score = max(diag, max(up, F));
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

    uint64_t align_start = ref_start;
    if (best_j > best_i) {
        align_start = ref_start + (best_j - best_i);
    }
    if (align_start >= ref_length) {
        align_start = (ref_length > 0) ? (ref_length - 1) : 0;
    }

    results[tid].read_id = read_id;
    results[tid].read_length = read_len;
    results[tid].position = align_start;
    results[tid].score = best_score;
    results[tid].cigar_length = 0; // Would generate from traceback
    results[tid].flag = 0;
    results[tid].mapping_quality = (best_score > 0) ? min(60, best_score / 2) : 0;
}

// CIGAR operations (currently unused placeholders)
enum class CigarOp { MATCH = 0, INSERTION = 1, DELETION = 2 };

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

// ===== PHASE 3: Host function for warp-optimized banded SW =====

// Host function: Launch warp-optimized banded Smith-Waterman
cudaError_t smith_waterman_align_banded_warp(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    const SWParams& params,
    AlignmentResult* results,
    uint32_t band_width,
    cudaStream_t stream
) {
    if (num_seeds == 0) return cudaSuccess;

    // One warp (32 threads) per alignment
    const uint32_t WARP_SIZE = 32;
    const uint32_t WARPS_PER_BLOCK = 8;  // 256 threads per block = 8 warps

    dim3 block_size(WARPS_PER_BLOCK * WARP_SIZE);  // 256 threads
    dim3 grid_size((num_seeds + WARPS_PER_BLOCK - 1) / WARPS_PER_BLOCK);

    // Calculate shared memory size per warp
    // Each warp needs: (band_width * 2 + 1) * 2 * sizeof(int32_t)
    uint32_t band_size = band_width * 2 + 1;
    size_t shared_mem_per_warp = band_size * 2 * sizeof(int32_t);
    size_t shared_mem_total = shared_mem_per_warp * WARPS_PER_BLOCK;

    smith_waterman_banded_warp_kernel<<<grid_size, block_size, shared_mem_total, stream>>>(
        reads.sequences,
        reads.offsets,
        reads.lengths,
        seeds,
        num_seeds,
        reference,
        ref_length,
        params,
        results,
        band_width
    );

    return cudaGetLastError();
}

// ===== Original host function (kept for compatibility) =====

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

    // Use banded warp kernel by default (Phase 3 optimization)
    // Falls back to original kernel if band width would be too large
    const uint32_t DEFAULT_BAND_WIDTH = 64;  // ±64 diagonal

    // Check if we can use banded kernel
    // Max shared memory is typically 48-96 KB, we use 64 as safe default
    uint32_t band_size = DEFAULT_BAND_WIDTH * 2 + 1;
    size_t shared_mem_per_warp = band_size * 2 * sizeof(int32_t);
    size_t shared_mem_total = shared_mem_per_warp * 8;  // 8 warps per block

    // Use banded warp kernel if shared memory requirement is reasonable
    if (shared_mem_total <= 32768) {  // 32 KB is conservative limit
        return smith_waterman_align_banded_warp(
            reads, seeds, num_seeds, reference, ref_length,
            params, results, DEFAULT_BAND_WIDTH, stream
        );
    }

    // Fallback to original kernel for very wide bands or low-memory GPUs
    dim3 block_size(256);
    dim3 grid_size((num_seeds + block_size.x - 1) / block_size.x);

    smith_waterman_kernel<<<grid_size, block_size, 0, stream>>>(
        reads.sequences,
        reads.offsets,
        reads.lengths,
        seeds,
        num_seeds,
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
