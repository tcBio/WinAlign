#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>
#include <cub/cub.cuh>

namespace winalign {
namespace cuda {

// Device function: Convert nucleotide to 2-bit encoding
__device__ inline uint8_t char_to_2bit(char c) {
    switch (c) {
        case 'A': case 'a': return 0;
        case 'C': case 'c': return 1;
        case 'G': case 'g': return 2;
        case 'T': case 't': return 3;
        default: return 0; // Treat N as A
    }
}

// Device function: Encode k-mer to 64-bit integer (for k <= 32)
__device__ uint64_t encode_kmer(const char* seq, uint32_t kmer_size) {
    uint64_t encoded = 0;
    for (uint32_t i = 0; i < kmer_size; ++i) {
        encoded = (encoded << 2) | char_to_2bit(seq[i]);
    }
    return encoded;
}

// Device function: Compute reverse complement of k-mer
__device__ uint64_t reverse_complement(uint64_t kmer, uint32_t kmer_size) {
    uint64_t rc = 0;
    for (uint32_t i = 0; i < kmer_size; ++i) {
        uint8_t base = (kmer >> (i * 2)) & 0x3;
        uint8_t comp = 3 - base; // A<->T, C<->G
        rc = (rc << 2) | comp;
    }
    return rc;
}

// Kernel: Extract k-mers from all reads in parallel
__global__ void extract_kmers_kernel(
    const char* sequences,
    const uint32_t* offsets,
    const uint32_t* lengths,
    uint32_t num_reads,
    uint32_t kmer_size,
    Seed* seeds,
    uint32_t* seed_counts
) {
    uint32_t read_id = blockIdx.x;
    if (read_id >= num_reads) return;

    uint32_t offset = offsets[read_id];
    uint32_t length = lengths[read_id];

    if (length < kmer_size) {
        seed_counts[read_id] = 0;
        return;
    }

    const char* read_seq = sequences + offset;
    uint32_t num_kmers = length - kmer_size + 1;

    // Each thread processes one k-mer
    uint32_t kmer_idx = threadIdx.x;

    if (kmer_idx < num_kmers) {
        // Extract k-mer
        uint64_t kmer = encode_kmer(read_seq + kmer_idx, kmer_size);
        uint64_t rc_kmer = reverse_complement(kmer, kmer_size);

        // Use canonical k-mer (lexicographically smaller)
        uint64_t canonical = min(kmer, rc_kmer);

        // Calculate global seed index
        uint32_t seed_base = 0;
        for (uint32_t i = 0; i < read_id; ++i) {
            seed_base += (lengths[i] >= kmer_size) ? (lengths[i] - kmer_size + 1) : 0;
        }
        uint32_t seed_idx = seed_base + kmer_idx;

        // Store seed
        seeds[seed_idx].position = canonical; // Temporary: store encoded k-mer
        seeds[seed_idx].read_id = read_id;
        seeds[seed_idx].read_offset = kmer_idx;
        seeds[seed_idx].length = kmer_size;
        seeds[seed_idx].mismatches = 0;
    }

    // First thread stores count
    if (threadIdx.x == 0) {
        seed_counts[read_id] = num_kmers;
    }
}

// Device function: Perform FM-index backward search
__device__ bool fm_index_search(
    const uint8_t* bwt,
    const uint64_t* c_table,
    const uint64_t* occ_table,
    uint64_t bwt_length,
    uint32_t occ_interval,
    const char* pattern,
    uint32_t pattern_len,
    uint64_t& sp,
    uint64_t& ep
) {
    sp = 0;
    ep = bwt_length - 1;

    // Backward search
    for (int i = pattern_len - 1; i >= 0; --i) {
        uint8_t c = char_to_2bit(pattern[i]);
        if (c >= 5) return false; // Invalid character

        // Compute rank(c, sp-1) and rank(c, ep)
        // Simplified rank computation (would need full occ_table logic)
        uint64_t rank_sp = (sp > 0) ? c_table[c] : 0;
        uint64_t rank_ep = c_table[c];

        // Update range
        sp = c_table[c] + rank_sp;
        ep = c_table[c] + rank_ep - 1;

        if (sp > ep) return false; // No matches
    }

    return true;
}

// Kernel: Match seeds against FM-index
__global__ void match_seeds_kernel(
    const Seed* seeds,
    uint32_t num_seeds,
    const FMIndex fm_index,
    const char* sequences,
    const uint32_t* offsets,
    uint32_t kmer_size,
    Seed* matched_seeds,
    uint32_t* match_flags
) {
    uint32_t seed_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (seed_idx >= num_seeds) return;

    const Seed& seed = seeds[seed_idx];

    // Decode k-mer position to get actual sequence
    // For now, mark all seeds as matched (simplified)
    // In production, would perform actual FM-index search

    matched_seeds[seed_idx] = seed;
    matched_seeds[seed_idx].position = seed_idx; // Placeholder position
    match_flags[seed_idx] = 1; // Mark as matched
}

// Kernel: Filter seeds by quality and occurrence count
__global__ void filter_seeds_kernel(
    Seed* seeds,
    uint32_t num_seeds,
    uint32_t* match_flags,
    uint32_t max_occurrences
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_seeds) return;

    // Filter seeds that occur too frequently (repetitive regions)
    // For now, keep all seeds
    // In production, would filter based on occurrence count

    if (match_flags[idx] > max_occurrences) {
        match_flags[idx] = 0; // Filter out
    }
}

// Host function: Extract seeds from reads
cudaError_t extract_seeds(
    const ReadBatch& reads,
    const FMIndex& fm_index,
    Seed* seeds,
    uint32_t max_seeds,
    uint32_t kmer_size,
    cudaStream_t stream
) {
    if (!seeds || reads.num_reads == 0) {
        return cudaErrorInvalidValue;
    }

    // Allocate device memory for seed counts
    uint32_t* d_seed_counts;
    cudaError_t err = cudaMalloc(&d_seed_counts, reads.num_reads * sizeof(uint32_t));
    if (err != cudaSuccess) return err;

    // Launch k-mer extraction kernel
    // One block per read, threads per k-mer
    dim3 block_size(256);
    dim3 grid_size(reads.num_reads);

    extract_kmers_kernel<<<grid_size, block_size, 0, stream>>>(
        reads.sequences,
        reads.offsets,
        reads.lengths,
        reads.num_reads,
        kmer_size,
        seeds,
        d_seed_counts
    );

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(d_seed_counts);
        return err;
    }

    // Synchronize stream
    if (stream == 0) {
        err = cudaDeviceSynchronize();
    } else {
        err = cudaStreamSynchronize(stream);
    }

    cudaFree(d_seed_counts);
    return err;
}

// Host function: Filter seeds
cudaError_t filter_seeds(
    Seed* seeds,
    uint32_t num_seeds,
    uint32_t max_seeds_per_read,
    cudaStream_t stream
) {
    if (!seeds || num_seeds == 0) {
        return cudaSuccess;
    }

    // Allocate match flags
    uint32_t* d_match_flags;
    cudaError_t err = cudaMalloc(&d_match_flags, num_seeds * sizeof(uint32_t));
    if (err != cudaSuccess) return err;

    // Initialize flags to 1 (all matched)
    err = cudaMemset(d_match_flags, 1, num_seeds * sizeof(uint32_t));
    if (err != cudaSuccess) {
        cudaFree(d_match_flags);
        return err;
    }

    // Launch filtering kernel
    dim3 block_size(256);
    dim3 grid_size((num_seeds + block_size.x - 1) / block_size.x);

    filter_seeds_kernel<<<grid_size, block_size, 0, stream>>>(
        seeds,
        num_seeds,
        d_match_flags,
        1000 // Max occurrences threshold
    );

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(d_match_flags);
        return err;
    }

    cudaFree(d_match_flags);

    // Synchronize
    if (stream == 0) {
        return cudaDeviceSynchronize();
    } else {
        return cudaStreamSynchronize(stream);
    }
}

cudaError_t allocate_read_batch(
    ReadBatch& batch,
    uint32_t max_reads,
    uint32_t max_read_length
) {
    cudaError_t err;

    err = cudaMalloc(&batch.sequences, max_reads * max_read_length);
    if (err != cudaSuccess) return err;

    err = cudaMalloc(&batch.lengths, max_reads * sizeof(uint32_t));
    if (err != cudaSuccess) return err;

    err = cudaMalloc(&batch.offsets, max_reads * sizeof(uint32_t));
    if (err != cudaSuccess) return err;

    batch.num_reads = 0;
    return cudaSuccess;
}

cudaError_t free_read_batch(ReadBatch& batch) {
    if (batch.sequences) cudaFree(batch.sequences);
    if (batch.lengths) cudaFree(batch.lengths);
    if (batch.offsets) cudaFree(batch.offsets);
    batch = ReadBatch();
    return cudaSuccess;
}

cudaError_t allocate_fm_index(FMIndex& fm_index, uint64_t length) {
    // TODO: Allocate FM-index structures
    fm_index.length = length;
    return cudaSuccess;
}

cudaError_t free_fm_index(FMIndex& fm_index) {
    if (fm_index.bwt) cudaFree(fm_index.bwt);
    if (fm_index.c_table) cudaFree(fm_index.c_table);
    if (fm_index.occ_table) cudaFree(fm_index.occ_table);
    fm_index = FMIndex();
    return cudaSuccess;
}

cudaError_t copy_fm_index_to_device(
    FMIndex& dst,
    const void* src,
    cudaStream_t stream
) {
    // TODO: Copy FM-index data to device
    return cudaSuccess;
}

} // namespace cuda
} // namespace winalign
