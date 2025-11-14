#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Kernel for k-mer extraction
__global__ void extract_kmers_kernel(
    const char* sequences,
    const uint32_t* offsets,
    const uint32_t* lengths,
    uint32_t num_reads,
    uint32_t kmer_size,
    Seed* seeds
) {
    // TODO: Implement k-mer extraction kernel
}

// Kernel for seed matching
__global__ void match_seeds_kernel(
    const Seed* seeds,
    uint32_t num_seeds,
    const FMIndex& fm_index,
    uint64_t* positions
) {
    // TODO: Implement FM-index based seed matching
}

cudaError_t extract_seeds(
    const ReadBatch& reads,
    const FMIndex& fm_index,
    Seed* seeds,
    uint32_t max_seeds,
    uint32_t kmer_size,
    cudaStream_t stream
) {
    // TODO: Launch kernels
    return cudaSuccess;
}

cudaError_t filter_seeds(
    Seed* seeds,
    uint32_t num_seeds,
    uint32_t max_seeds_per_read,
    cudaStream_t stream
) {
    // TODO: Filter and rank seeds
    return cudaSuccess;
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
