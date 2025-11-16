#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>
#include <cub/cub.cuh>

namespace winalign {
namespace cuda {

__device__ inline uint8_t char_to_2bit(char c) {
    switch (c) {
        case 'A': case 'a': return 0;
        case 'C': case 'c': return 1;
        case 'G': case 'g': return 2;
        case 'T': case 't': return 3;
        default: return 0;
    }
}

__device__ inline uint8_t char_to_base(char c) {
    switch (c) {
        case 'A': case 'a': return 0;
        case 'C': case 'c': return 1;
        case 'G': case 'g': return 2;
        case 'T': case 't': return 3;
        default: return 4;
    }
}

__device__ uint64_t occ_rank(
    const FMIndex& fm_index,
    uint8_t base,
    int64_t pos
) {
    if (base >= 5 || fm_index.length == 0) return 0;
    if (pos < 0) return 0;
    if (pos >= static_cast<int64_t>(fm_index.length)) {
        pos = static_cast<int64_t>(fm_index.length) - 1;
    }

    uint64_t checkpoint = static_cast<uint64_t>(pos) / fm_index.occ_interval;
    uint64_t count = 0;
    if (checkpoint > 0) {
        count = fm_index.occ_table[(checkpoint - 1) * 5 + base];
    }
    uint64_t start = checkpoint * fm_index.occ_interval;
    for (uint64_t i = start; i <= static_cast<uint64_t>(pos); ++i) {
        if (fm_index.bwt[i] == base) {
            count++;
        }
    }
    return count;
}

__device__ bool backward_search(
    const FMIndex& fm_index,
    const char* seq,
    uint32_t start,
    uint32_t kmer_size,
    uint32_t read_length,
    uint64_t& sp,
    uint64_t& ep
) {
    if (start + kmer_size > read_length || fm_index.length == 0) {
        return false;
    }

    sp = 0;
    ep = fm_index.length - 1;
    for (int32_t i = static_cast<int32_t>(kmer_size) - 1; i >= 0; --i) {
        uint8_t base = char_to_base(seq[start + i]);
        if (base >= 4) {
            return false;
        }

        uint64_t occ_sp = (sp == 0)
            ? 0
            : occ_rank(fm_index, base, static_cast<int64_t>(sp) - 1);
        uint64_t occ_ep = occ_rank(fm_index, base, static_cast<int64_t>(ep));
        sp = fm_index.c_table[base] + occ_sp;
        ep = fm_index.c_table[base] + occ_ep - 1;
        if (sp > ep) {
            return false;
        }
    }
    return sp <= ep;
}

__global__ void fm_seed_kernel(
    const ReadBatch reads,
    const FMIndex fm_index,
    Seed* tmp_seeds,
    uint32_t max_seeds_per_read,
    uint32_t kmer_size,
    uint32_t step,
    uint32_t max_hits_per_seed,
    uint32_t* seed_counts
) {
    uint32_t read_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (read_id >= reads.num_reads) return;

    const char* seq = reads.sequences + reads.offsets[read_id];
    uint32_t length = reads.lengths[read_id];

    if (length < kmer_size) {
        seed_counts[read_id] = 0;
        return;
    }

    uint32_t stride = max(step, 1u);
    uint32_t base_offset = read_id * max_seeds_per_read;
    uint32_t written = 0;

    for (uint32_t start = 0;
         start + kmer_size <= length && written < max_seeds_per_read;
         start += stride) {
        uint64_t sp = 0, ep = 0;
        if (!backward_search(fm_index, seq, start, kmer_size, length, sp, ep)) {
            continue;
        }

        uint64_t hits = (ep >= sp) ? (ep - sp + 1) : 0;
        if (hits > max_hits_per_seed) {
            hits = max_hits_per_seed;
        }

        for (uint64_t h = 0; h < hits && written < max_seeds_per_read; ++h) {
            uint64_t sa_pos = fm_index.suffix_array[sp + h];
            Seed seed{};
            seed.position = sa_pos;
            seed.read_id = read_id;
            seed.read_offset = start;
            seed.length = kmer_size;
            seed.mismatches = 0;
            tmp_seeds[base_offset + written] = seed;
            written++;
        }
    }

    seed_counts[read_id] = written;
}

__global__ void compact_seeds_kernel(
    const Seed* src,
    Seed* dst,
    const uint32_t* offsets,
    const uint32_t* counts,
    uint32_t max_seeds_per_read,
    uint32_t num_reads
) {
    uint32_t read_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (read_id >= num_reads) return;

    uint32_t count = counts[read_id];
    if (count == 0) return;

    uint32_t offset = offsets[read_id];
    uint32_t base = read_id * max_seeds_per_read;
    for (uint32_t i = 0; i < count; ++i) {
        dst[offset + i] = src[base + i];
    }
}

cudaError_t generate_gpu_seeds(
    const ReadBatch& reads,
    const FMIndex& fm_index,
    Seed* seeds,
    uint32_t max_seeds_per_read,
    uint32_t kmer_size,
    uint32_t step,
    uint32_t& out_total_seeds,
    cudaStream_t stream
) {
    out_total_seeds = 0;
    if (reads.num_reads == 0 || kmer_size == 0) {
        return cudaSuccess;
    }

    uint32_t num_reads = reads.num_reads;
    uint64_t total_slots = static_cast<uint64_t>(num_reads) * max_seeds_per_read;

    Seed* d_tmp_seeds = nullptr;
    uint32_t* d_counts = nullptr;
    uint32_t* d_offsets = nullptr;
    void* d_temp_storage = nullptr;
    size_t temp_bytes = 0;
    uint32_t last_offset = 0;
    uint32_t last_count = 0;

    const uint32_t block_size = 128;
    dim3 block(block_size);
    dim3 grid((num_reads + block_size - 1) / block_size);
    const uint32_t max_hits_per_seed = 8;

    cudaError_t err = cudaMalloc(&d_tmp_seeds, total_slots * sizeof(Seed));
    if (err != cudaSuccess) goto cleanup;
    err = cudaMalloc(&d_counts, num_reads * sizeof(uint32_t));
    if (err != cudaSuccess) goto cleanup;
    err = cudaMalloc(&d_offsets, num_reads * sizeof(uint32_t));
    if (err != cudaSuccess) goto cleanup;
    err = cudaMemsetAsync(d_counts, 0, num_reads * sizeof(uint32_t), stream);
    if (err != cudaSuccess) goto cleanup;

    fm_seed_kernel<<<grid, block, 0, stream>>>(
        reads,
        fm_index,
        d_tmp_seeds,
        max_seeds_per_read,
        kmer_size,
        step,
        max_hits_per_seed,
        d_counts
    );
    err = cudaGetLastError();
    if (err != cudaSuccess) goto cleanup;

    err = cub::DeviceScan::ExclusiveSum(
        nullptr,
        temp_bytes,
        d_counts,
        d_offsets,
        num_reads,
        stream);
    if (err != cudaSuccess) goto cleanup;

    err = cudaMalloc(&d_temp_storage, temp_bytes);
    if (err != cudaSuccess) goto cleanup;

    err = cub::DeviceScan::ExclusiveSum(
        d_temp_storage,
        temp_bytes,
        d_counts,
        d_offsets,
        num_reads,
        stream);
    if (err != cudaSuccess) goto cleanup;

    err = cudaMemcpyAsync(
        &last_offset,
        d_offsets + (num_reads - 1),
        sizeof(uint32_t),
        cudaMemcpyDeviceToHost,
        stream);
    if (err != cudaSuccess) goto cleanup;
    err = cudaMemcpyAsync(
        &last_count,
        d_counts + (num_reads - 1),
        sizeof(uint32_t),
        cudaMemcpyDeviceToHost,
        stream);
    if (err != cudaSuccess) goto cleanup;
    err = cudaStreamSynchronize(stream);
    if (err != cudaSuccess) goto cleanup;

    out_total_seeds = last_offset + last_count;
    if (out_total_seeds == 0) {
        err = cudaSuccess;
        goto cleanup;
    }

    compact_seeds_kernel<<<grid, block, 0, stream>>>(
        d_tmp_seeds,
        seeds,
        d_offsets,
        d_counts,
        max_seeds_per_read,
        num_reads);
    err = cudaGetLastError();

cleanup:
    if (d_tmp_seeds) cudaFree(d_tmp_seeds);
    if (d_counts) cudaFree(d_counts);
    if (d_offsets) cudaFree(d_offsets);
    if (d_temp_storage) cudaFree(d_temp_storage);
    return err;
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

cudaError_t allocate_fm_index(
    FMIndex& fm_index,
    uint64_t length,
    size_t occ_entries,
    size_t suffix_length
) {
    cudaError_t err;

    err = cudaMalloc(&fm_index.bwt, length * sizeof(uint8_t));
    if (err != cudaSuccess) return err;

    err = cudaMalloc(&fm_index.c_table, 5 * sizeof(uint64_t));
    if (err != cudaSuccess) return err;

    err = cudaMalloc(&fm_index.occ_table, occ_entries * sizeof(uint64_t));
    if (err != cudaSuccess) return err;

    err = cudaMalloc(&fm_index.suffix_array, suffix_length * sizeof(uint64_t));
    if (err != cudaSuccess) return err;

    fm_index.length = length;
    fm_index.occ_entries = occ_entries;
    fm_index.suffix_length = suffix_length;
    return cudaSuccess;
}

cudaError_t free_fm_index(FMIndex& fm_index) {
    if (fm_index.bwt) cudaFree(fm_index.bwt);
    if (fm_index.c_table) cudaFree(fm_index.c_table);
    if (fm_index.occ_table) cudaFree(fm_index.occ_table);
    if (fm_index.suffix_array) cudaFree(fm_index.suffix_array);
    fm_index = FMIndex();
    return cudaSuccess;
}

cudaError_t copy_fm_index_to_device(
    FMIndex& dst,
    const HostFMIndexView& src,
    cudaStream_t stream
) {
    if (!src.bwt || !src.c_table || !src.occ_table || !src.suffix_array) {
        return cudaErrorInvalidValue;
    }

    dst.length = src.length;
    dst.occ_entries = src.occ_entries;
    dst.suffix_length = src.suffix_length;
    dst.occ_interval = src.occ_interval;

    cudaError_t err = cudaMemcpyAsync(
        dst.bwt,
        src.bwt,
        src.length * sizeof(uint8_t),
        cudaMemcpyHostToDevice,
        stream);
    if (err != cudaSuccess) return err;

    err = cudaMemcpyAsync(
        dst.c_table,
        src.c_table,
        5 * sizeof(uint64_t),
        cudaMemcpyHostToDevice,
        stream);
    if (err != cudaSuccess) return err;

    const size_t occ_bytes = src.occ_entries * sizeof(uint64_t);
    err = cudaMemcpyAsync(
        dst.occ_table,
        src.occ_table,
        occ_bytes,
        cudaMemcpyHostToDevice,
        stream);
    if (err != cudaSuccess) return err;

    const size_t sa_bytes = src.suffix_length * sizeof(uint64_t);
    err = cudaMemcpyAsync(
        dst.suffix_array,
        src.suffix_array,
        sa_bytes,
        cudaMemcpyHostToDevice,
        stream);
    if (err != cudaSuccess) return err;

    if (stream == 0) {
        return cudaDeviceSynchronize();
    }
    return cudaStreamSynchronize(stream);
}

} // namespace cuda
} // namespace winalign
