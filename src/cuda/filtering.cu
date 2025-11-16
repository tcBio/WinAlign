#include "winalign/cuda/filtering.cuh"
#include <cuda_runtime.h>
#include <thrust/sort.h>
#include <thrust/remove.h>
#include <thrust/device_ptr.h>
#include <thrust/execution_policy.h>
#include <cub/cub.cuh>

namespace winalign {
namespace cuda {

// SAM flags
constexpr uint16_t SAM_FLAG_SECONDARY = 0x100;
constexpr uint16_t SAM_FLAG_SUPPLEMENTARY = 0x800;
constexpr uint16_t SAM_FLAG_DUPLICATE = 0x400;
constexpr uint16_t SAM_FLAG_PROPER_PAIR = 0x2;

// Predicate used when compacting results
struct IsFiltered {
    __host__ __device__
    bool operator()(const AlignmentResult& r) const {
        return r.score == 0 || (r.flag & SAM_FLAG_DUPLICATE);
    }
};

// Device function: Check if alignment passes quality filter
__device__ inline bool passes_quality_filter(
    const AlignmentResult& result,
    const FilterParams& params
) {
    // Check mapping quality
    if (result.mapping_quality < params.min_mapping_quality) {
        return false;
    }

    // Check alignment score
    if (static_cast<uint32_t>(result.score) < params.min_alignment_score) {
        return false;
    }

    // Filter secondary alignments if requested
    if (params.filter_secondary && (result.flag & SAM_FLAG_SECONDARY)) {
        return false;
    }

    // Filter supplementary alignments if requested
    if (params.filter_supplementary && (result.flag & SAM_FLAG_SUPPLEMENTARY)) {
        return false;
    }

    return true;
}

// Kernel: Quality filtering
__global__ void filter_quality_kernel(
    AlignmentResult* results,
    uint32_t num_results,
    FilterParams params,
    uint32_t* filter_flags
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_results) return;

    // Mark alignment as passed (1) or filtered (0)
    filter_flags[idx] = passes_quality_filter(results[idx], params) ? 1 : 0;
}

// Kernel: Mark duplicates
__global__ void mark_duplicates_kernel(
    AlignmentResult* results,
    uint32_t num_results,
    uint32_t* duplicate_flags
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_results) return;

    // Check if this alignment is a duplicate
    // Simple approach: check if previous alignment has same position
    if (idx > 0) {
        const AlignmentResult& curr = results[idx];
        const AlignmentResult& prev = results[idx - 1];

        // Same position and read_id indicates duplicate
        if (curr.position == prev.position && curr.read_id == prev.read_id) {
            // Keep higher quality alignment
            if (curr.mapping_quality < prev.mapping_quality) {
                duplicate_flags[idx] = 1;
                results[idx].flag |= SAM_FLAG_DUPLICATE;
            }
        }
    }
}

// Kernel: Validate paired-end alignments
__global__ void validate_pairs_kernel(
    AlignmentResult* results1,
    AlignmentResult* results2,
    uint32_t num_pairs,
    uint32_t min_insert_size,
    uint32_t max_insert_size,
    uint32_t* pair_flags
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_pairs) return;

    const AlignmentResult& r1 = results1[idx];
    const AlignmentResult& r2 = results2[idx];

    // Both must be aligned
    if (r1.score == 0 || r2.score == 0) {
        pair_flags[idx] = 0;
        return;
    }

    // Calculate insert size
    uint64_t start = min(r1.position, r2.position);
    uint64_t end = max(r1.position, r2.position);
    uint64_t insert_size = end - start;

    // Check insert size range
    bool valid = (insert_size >= min_insert_size && insert_size <= max_insert_size);
    pair_flags[idx] = valid ? 1 : 0;

    // Set proper pair flag
    if (valid) {
        results1[idx].flag |= SAM_FLAG_PROPER_PAIR;
        results2[idx].flag |= SAM_FLAG_PROPER_PAIR;
    }
}

// Kernel: Compute statistics using block reduction
__global__ void compute_stats_kernel(
    const AlignmentResult* results,
    uint32_t num_results,
    uint32_t* total_alignments,
    uint32_t* primary_alignments,
    uint32_t* secondary_alignments,
    uint32_t* duplicates,
    uint64_t* sum_mapq,
    uint64_t* sum_score
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;

    // Per-thread accumulators
    uint32_t local_total = 0;
    uint32_t local_primary = 0;
    uint32_t local_secondary = 0;
    uint32_t local_duplicates = 0;
    uint64_t local_mapq = 0;
    uint64_t local_score = 0;

    // Process alignments
    if (idx < num_results) {
        const AlignmentResult& r = results[idx];

        local_total = 1;
        local_mapq = r.mapping_quality;
        local_score = r.score;

        if (r.flag & SAM_FLAG_SECONDARY) {
            local_secondary = 1;
        } else {
            local_primary = 1;
        }

        if (r.flag & SAM_FLAG_DUPLICATE) {
            local_duplicates = 1;
        }
    }

    // Block-level reduction using CUB
    typedef cub::BlockReduce<uint32_t, 256> BlockReduceUint32;
    typedef cub::BlockReduce<uint64_t, 256> BlockReduceUint64;
    __shared__ typename BlockReduceUint32::TempStorage temp_storage_uint32;
    __shared__ typename BlockReduceUint64::TempStorage temp_storage_uint64;

    // Reduce total alignments
    uint32_t block_total = BlockReduceUint32(temp_storage_uint32).Sum(local_total);
    __syncthreads();

    // Reduce primary alignments
    uint32_t block_primary = BlockReduceUint32(temp_storage_uint32).Sum(local_primary);
    __syncthreads();

    // Reduce secondary alignments
    uint32_t block_secondary = BlockReduceUint32(temp_storage_uint32).Sum(local_secondary);
    __syncthreads();

    // Reduce duplicates
    uint32_t block_duplicates = BlockReduceUint32(temp_storage_uint32).Sum(local_duplicates);
    __syncthreads();

    // Reduce MAPQ sum
    uint64_t block_mapq = BlockReduceUint64(temp_storage_uint64).Sum(local_mapq);
    __syncthreads();

    // Reduce score sum
    uint64_t block_score = BlockReduceUint64(temp_storage_uint64).Sum(local_score);

    // First thread writes block results
    if (threadIdx.x == 0) {
        atomicAdd(total_alignments, block_total);
        atomicAdd(primary_alignments, block_primary);
        atomicAdd(secondary_alignments, block_secondary);
        atomicAdd(duplicates, block_duplicates);
        atomicAdd((unsigned long long*)sum_mapq, (unsigned long long)block_mapq);
        atomicAdd((unsigned long long*)sum_score, (unsigned long long)block_score);
    }
}

// Functor for thrust::remove_if
struct is_filtered {
    uint32_t* filter_flags;

    __host__ __device__
    is_filtered(uint32_t* flags) : filter_flags(flags) {}

    __host__ __device__
    bool operator()(const AlignmentResult& result) const {
        // Find index of this result (this is a simplified approach)
        // In production, we'd use a paired zip iterator
        return false; // Placeholder
    }
};

// Comparator for sorting by coordinate
struct compare_by_coordinate {
    __host__ __device__
    bool operator()(const AlignmentResult& a, const AlignmentResult& b) const {
        // Sort by position, then by read_id
        if (a.position != b.position) {
            return a.position < b.position;
        }
        return a.read_id < b.read_id;
    }
};

// Host function: Filter by quality
uint32_t filter_by_quality(
    AlignmentResult* results,
    uint32_t num_results,
    const FilterParams& params,
    cudaStream_t stream
) {
    if (num_results == 0) return 0;

    // Allocate filter flags
    uint32_t* d_filter_flags;
    cudaError_t err = cudaMalloc(&d_filter_flags, num_results * sizeof(uint32_t));
    if (err != cudaSuccess) {
        return 0;
    }

    // Initialize to zero
    cudaMemset(d_filter_flags, 0, num_results * sizeof(uint32_t));

    // Launch kernel
    dim3 block_size(256);
    dim3 grid_size((num_results + block_size.x - 1) / block_size.x);

    filter_quality_kernel<<<grid_size, block_size, 0, stream>>>(
        results,
        num_results,
        params,
        d_filter_flags
    );

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(d_filter_flags);
        return 0;
    }

    // Count passing alignments using CUB reduction
    uint32_t* d_num_passed;
    cudaMalloc(&d_num_passed, sizeof(uint32_t));
    cudaMemset(d_num_passed, 0, sizeof(uint32_t));

    void* d_temp_storage = nullptr;
    size_t temp_storage_bytes = 0;

    // Get temp storage size
    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_filter_flags,
        d_num_passed,
        num_results,
        stream
    );

    // Allocate temp storage
    cudaMalloc(&d_temp_storage, temp_storage_bytes);

    // Run reduction
    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_filter_flags,
        d_num_passed,
        num_results,
        stream
    );

    // Copy result back
    uint32_t num_passed;
    cudaMemcpyAsync(&num_passed, d_num_passed, sizeof(uint32_t),
                    cudaMemcpyDeviceToHost, stream);

    // Synchronize
    if (stream == 0) {
        cudaDeviceSynchronize();
    } else {
        cudaStreamSynchronize(stream);
    }

    // Cleanup
    cudaFree(d_filter_flags);
    cudaFree(d_num_passed);
    cudaFree(d_temp_storage);

    return num_passed;
}

// Host function: Mark duplicates
uint32_t mark_duplicates(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream
) {
    if (num_results == 0) return 0;

    // Sort by coordinate first
    sort_by_coordinate(results, num_results, stream);

    // Allocate duplicate flags
    uint32_t* d_duplicate_flags;
    cudaError_t err = cudaMalloc(&d_duplicate_flags, num_results * sizeof(uint32_t));
    if (err != cudaSuccess) {
        return 0;
    }

    cudaMemset(d_duplicate_flags, 0, num_results * sizeof(uint32_t));

    // Launch kernel
    dim3 block_size(256);
    dim3 grid_size((num_results + block_size.x - 1) / block_size.x);

    mark_duplicates_kernel<<<grid_size, block_size, 0, stream>>>(
        results,
        num_results,
        d_duplicate_flags
    );

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(d_duplicate_flags);
        return 0;
    }

    // Count duplicates
    uint32_t* d_num_duplicates;
    cudaMalloc(&d_num_duplicates, sizeof(uint32_t));
    cudaMemset(d_num_duplicates, 0, sizeof(uint32_t));

    void* d_temp_storage = nullptr;
    size_t temp_storage_bytes = 0;

    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_duplicate_flags,
        d_num_duplicates,
        num_results,
        stream
    );

    cudaMalloc(&d_temp_storage, temp_storage_bytes);

    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_duplicate_flags,
        d_num_duplicates,
        num_results,
        stream
    );

    uint32_t num_duplicates;
    cudaMemcpyAsync(&num_duplicates, d_num_duplicates, sizeof(uint32_t),
                    cudaMemcpyDeviceToHost, stream);

    if (stream == 0) {
        cudaDeviceSynchronize();
    } else {
        cudaStreamSynchronize(stream);
    }

    cudaFree(d_duplicate_flags);
    cudaFree(d_num_duplicates);
    cudaFree(d_temp_storage);

    return num_duplicates;
}

// Host function: Validate pairs
uint32_t validate_pairs(
    AlignmentResult* results1,
    AlignmentResult* results2,
    uint32_t num_pairs,
    uint32_t min_insert_size,
    uint32_t max_insert_size,
    cudaStream_t stream
) {
    if (num_pairs == 0) return 0;

    // Allocate pair flags
    uint32_t* d_pair_flags;
    cudaError_t err = cudaMalloc(&d_pair_flags, num_pairs * sizeof(uint32_t));
    if (err != cudaSuccess) {
        return 0;
    }

    cudaMemset(d_pair_flags, 0, num_pairs * sizeof(uint32_t));

    // Launch kernel
    dim3 block_size(256);
    dim3 grid_size((num_pairs + block_size.x - 1) / block_size.x);

    validate_pairs_kernel<<<grid_size, block_size, 0, stream>>>(
        results1,
        results2,
        num_pairs,
        min_insert_size,
        max_insert_size,
        d_pair_flags
    );

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(d_pair_flags);
        return 0;
    }

    // Count proper pairs
    uint32_t* d_num_proper_pairs;
    cudaMalloc(&d_num_proper_pairs, sizeof(uint32_t));
    cudaMemset(d_num_proper_pairs, 0, sizeof(uint32_t));

    void* d_temp_storage = nullptr;
    size_t temp_storage_bytes = 0;

    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_pair_flags,
        d_num_proper_pairs,
        num_pairs,
        stream
    );

    cudaMalloc(&d_temp_storage, temp_storage_bytes);

    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_pair_flags,
        d_num_proper_pairs,
        num_pairs,
        stream
    );

    uint32_t num_proper_pairs;
    cudaMemcpyAsync(&num_proper_pairs, d_num_proper_pairs, sizeof(uint32_t),
                    cudaMemcpyDeviceToHost, stream);

    if (stream == 0) {
        cudaDeviceSynchronize();
    } else {
        cudaStreamSynchronize(stream);
    }

    cudaFree(d_pair_flags);
    cudaFree(d_num_proper_pairs);
    cudaFree(d_temp_storage);

    return num_proper_pairs;
}

// Host function: Sort by coordinate
cudaError_t sort_by_coordinate(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream
) {
    if (num_results == 0) return cudaSuccess;

    // Use thrust::sort with custom comparator
    thrust::device_ptr<AlignmentResult> d_results(results);

    try {
        if (stream == 0) {
            thrust::sort(
                d_results,
                d_results + num_results,
                compare_by_coordinate()
            );
        } else {
            thrust::sort(
                thrust::cuda::par.on(stream),
                d_results,
                d_results + num_results,
                compare_by_coordinate()
            );
        }
    } catch (thrust::system_error& e) {
        return cudaErrorUnknown;
    }

    return cudaGetLastError();
}

// Host function: Compact results
uint32_t compact_results(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream
) {
    if (num_results == 0) return 0;

    // For simplicity, we'll remove alignments with score == 0
    // In production, this would use a separate filter flags array
    thrust::device_ptr<AlignmentResult> d_results(results);
    auto new_end = thrust::remove_if(
        thrust::cuda::par.on(stream),
        d_results,
        d_results + num_results,
        IsFiltered()
    );

    uint32_t new_count = thrust::distance(d_results, new_end);

    if (stream == 0) {
        cudaDeviceSynchronize();
    } else {
        cudaStreamSynchronize(stream);
    }

    return new_count;
}

// Host function: Compute statistics
cudaError_t compute_statistics(
    const AlignmentResult* results,
    uint32_t num_results,
    AlignmentStats* stats,
    cudaStream_t stream
) {
    if (!results || !stats || num_results == 0) {
        return cudaErrorInvalidValue;
    }

    // Allocate device memory for accumulators
    uint32_t* d_total;
    uint32_t* d_primary;
    uint32_t* d_secondary;
    uint32_t* d_duplicates;
    uint64_t* d_sum_mapq;
    uint64_t* d_sum_score;

    cudaMalloc(&d_total, sizeof(uint32_t));
    cudaMalloc(&d_primary, sizeof(uint32_t));
    cudaMalloc(&d_secondary, sizeof(uint32_t));
    cudaMalloc(&d_duplicates, sizeof(uint32_t));
    cudaMalloc(&d_sum_mapq, sizeof(uint64_t));
    cudaMalloc(&d_sum_score, sizeof(uint64_t));

    cudaMemset(d_total, 0, sizeof(uint32_t));
    cudaMemset(d_primary, 0, sizeof(uint32_t));
    cudaMemset(d_secondary, 0, sizeof(uint32_t));
    cudaMemset(d_duplicates, 0, sizeof(uint32_t));
    cudaMemset(d_sum_mapq, 0, sizeof(uint64_t));
    cudaMemset(d_sum_score, 0, sizeof(uint64_t));

    // Launch kernel
    dim3 block_size(256);
    dim3 grid_size((num_results + block_size.x - 1) / block_size.x);

    compute_stats_kernel<<<grid_size, block_size, 0, stream>>>(
        results,
        num_results,
        d_total,
        d_primary,
        d_secondary,
        d_duplicates,
        d_sum_mapq,
        d_sum_score
    );

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(d_total);
        cudaFree(d_primary);
        cudaFree(d_secondary);
        cudaFree(d_duplicates);
        cudaFree(d_sum_mapq);
        cudaFree(d_sum_score);
        return err;
    }

    // Copy results back to host
    uint32_t h_total, h_primary, h_secondary, h_duplicates;
    uint64_t h_sum_mapq, h_sum_score;

    cudaMemcpyAsync(&h_total, d_total, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream);
    cudaMemcpyAsync(&h_primary, d_primary, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream);
    cudaMemcpyAsync(&h_secondary, d_secondary, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream);
    cudaMemcpyAsync(&h_duplicates, d_duplicates, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream);
    cudaMemcpyAsync(&h_sum_mapq, d_sum_mapq, sizeof(uint64_t), cudaMemcpyDeviceToHost, stream);
    cudaMemcpyAsync(&h_sum_score, d_sum_score, sizeof(uint64_t), cudaMemcpyDeviceToHost, stream);

    // Synchronize
    if (stream == 0) {
        cudaDeviceSynchronize();
    } else {
        cudaStreamSynchronize(stream);
    }

    // Fill statistics structure
    stats->total_alignments = h_total;
    stats->primary_alignments = h_primary;
    stats->secondary_alignments = h_secondary;
    stats->duplicates = h_duplicates;
    stats->mean_mapping_quality = (h_total > 0) ? static_cast<float>(h_sum_mapq) / h_total : 0.0f;
    stats->mean_alignment_score = (h_total > 0) ? static_cast<float>(h_sum_score) / h_total : 0.0f;

    // Cleanup
    cudaFree(d_total);
    cudaFree(d_primary);
    cudaFree(d_secondary);
    cudaFree(d_duplicates);
    cudaFree(d_sum_mapq);
    cudaFree(d_sum_score);

    return cudaSuccess;
}

} // namespace cuda
} // namespace winalign
