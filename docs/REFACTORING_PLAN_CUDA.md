# CUDA Files Refactoring Plan

**Scope**: Refactor 3 oversized CUDA kernel files
**Timeline**: Week 2 (2025-11-27 to 2025-12-03)
**Priority**: 🟡 HIGH - After pipeline.cpp refactoring

---

## 📊 Files to Refactor

```
❌ src/cuda/filtering.cu     649 lines (30% over 500-line limit)
❌ src/cuda/alignment.cu      566 lines (13% over 500-line limit)
❌ src/cuda/seeding.cu         565 lines (13% over 500-line limit)
```

---

## 🎯 File 1: filtering.cu (649 → 450 lines)

### Current Structure

```
Lines   1-50   : Includes, namespace, constants
Lines  51-150  : Quality filtering kernel + device functions
Lines 151-250  : Duplicate marking kernel + device functions
Lines 251-350  : Paired-end validation kernel
Lines 351-450  : Statistics computation kernels
Lines 451-550  : Sorting and compaction (Thrust/CUB)
Lines 551-649  : Host wrapper functions
```

### New Structure

```
src/cuda/
├── filtering.cu                    (200 lines) - Main API + host wrappers
├── filtering_quality.cu            (150 lines) - Quality filtering kernels
├── filtering_duplicates.cu         (150 lines) - Duplicate marking kernels
├── filtering_pairs.cu              (150 lines) - Paired-end validation
└── filtering_device.cuh            (100 lines) - Shared device functions
```

### Extraction Details

**filtering_device.cuh** (100 lines) - Shared Helpers
```cuda
#pragma once

namespace winalign {
namespace cuda {
namespace filtering_internal {

// Common device functions used across all filtering kernels

__device__ inline bool is_high_quality(const AlignmentResult& result, uint32_t min_mapq) {
    return result.mapq >= min_mapq && result.score > 0;
}

__device__ inline uint64_t coord_hash(uint32_t pos, bool is_reverse) {
    // Hash for duplicate detection
    return (static_cast<uint64_t>(pos) << 1) | (is_reverse ? 1 : 0);
}

__device__ inline bool is_proper_pair(
    const AlignmentResult& r1,
    const AlignmentResult& r2,
    uint32_t max_insert_size
) {
    // Paired-end validation logic
    // ... implementation
}

} // namespace filtering_internal
} // namespace cuda
} // namespace winalign
```

**filtering_quality.cu** (150 lines) - Quality Filtering
```cuda
#include "winalign/cuda/filtering.cuh"
#include "filtering_device.cuh"

namespace winalign {
namespace cuda {

// Quality filtering kernel
__global__ void filter_quality_kernel(
    const AlignmentResult* results,
    uint32_t num_results,
    uint32_t min_mapq,
    uint32_t min_score,
    bool* keep_flags
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= num_results) return;

    const AlignmentResult& result = results[tid];

    // Use device helper
    bool pass = filtering_internal::is_high_quality(result, min_mapq);
    pass = pass && (result.score >= static_cast<int32_t>(min_score));

    keep_flags[tid] = pass;
}

// Additional quality-related kernels...

// Host wrapper
cudaError_t filter_by_quality(
    const AlignmentResult* d_results,
    uint32_t num_results,
    uint32_t min_mapq,
    uint32_t min_score,
    bool* d_keep_flags,
    cudaStream_t stream
) {
    dim3 block(256);
    dim3 grid((num_results + block.x - 1) / block.x);

    filter_quality_kernel<<<grid, block, 0, stream>>>(
        d_results, num_results, min_mapq, min_score, d_keep_flags
    );

    return cudaGetLastError();
}

} // namespace cuda
} // namespace winalign
```

**filtering_duplicates.cu** (150 lines) - Duplicate Marking
```cuda
#include "winalign/cuda/filtering.cuh"
#include "filtering_device.cuh"

namespace winalign {
namespace cuda {

// Duplicate marking kernel
__global__ void mark_duplicates_kernel(
    AlignmentResult* results,
    uint32_t num_results,
    bool* duplicate_flags
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= num_results) return;

    AlignmentResult& result = results[tid];

    // Use device helper for hashing
    uint64_t hash = filtering_internal::coord_hash(result.position, result.is_reverse);

    // Mark duplicates based on hash (simplified)
    // Real implementation would use atomics and careful logic
    // ...
}

// Host wrapper
cudaError_t mark_duplicates(
    AlignmentResult* d_results,
    uint32_t num_results,
    bool* d_duplicate_flags,
    cudaStream_t stream
) {
    // Implementation
}

} // namespace cuda
} // namespace winalign
```

**filtering_pairs.cu** (150 lines) - Paired-End Validation
```cuda
#include "winalign/cuda/filtering.cuh"
#include "filtering_device.cuh"

namespace winalign {
namespace cuda {

// Paired-end validation kernel
__global__ void validate_pairs_kernel(
    const AlignmentResult* r1_results,
    const AlignmentResult* r2_results,
    uint32_t num_pairs,
    uint32_t max_insert_size,
    bool* proper_pair_flags
) {
    uint32_t tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= num_pairs) return;

    const AlignmentResult& r1 = r1_results[tid];
    const AlignmentResult& r2 = r2_results[tid];

    // Use device helper
    bool proper = filtering_internal::is_proper_pair(r1, r2, max_insert_size);
    proper_pair_flags[tid] = proper;
}

// Host wrapper
cudaError_t validate_pairs(
    const AlignmentResult* d_r1_results,
    const AlignmentResult* d_r2_results,
    uint32_t num_pairs,
    uint32_t max_insert_size,
    bool* d_proper_pair_flags,
    cudaStream_t stream
) {
    // Implementation
}

} // namespace cuda
} // namespace winalign
```

**filtering.cu** (200 lines) - Main API
```cuda
#include "winalign/cuda/filtering.cuh"
#include <cub/cub.cuh>

namespace winalign {
namespace cuda {

// Forward declarations for functions in other files
extern cudaError_t filter_by_quality(/* ... */);
extern cudaError_t mark_duplicates(/* ... */);
extern cudaError_t validate_pairs(/* ... */);

// Main filtering pipeline (combines all filters)
cudaError_t filter_alignments(
    AlignmentResult* d_results,
    uint32_t num_results,
    const FilterParams& params,
    uint32_t& out_num_kept,
    cudaStream_t stream
) {
    // Allocate temporary flags
    bool* d_quality_flags = nullptr;
    bool* d_duplicate_flags = nullptr;
    bool* d_final_flags = nullptr;

    cudaMalloc(&d_quality_flags, num_results * sizeof(bool));
    cudaMalloc(&d_duplicate_flags, num_results * sizeof(bool));
    cudaMalloc(&d_final_flags, num_results * sizeof(bool));

    // Step 1: Quality filtering
    filter_by_quality(d_results, num_results, params.min_mapq,
                      params.min_score, d_quality_flags, stream);

    // Step 2: Duplicate marking
    mark_duplicates(d_results, num_results, d_duplicate_flags, stream);

    // Step 3: Combine flags (quality AND !duplicate)
    // Use Thrust or custom kernel to combine

    // Step 4: Compact results using CUB
    // ... compaction logic

    // Cleanup
    cudaFree(d_quality_flags);
    cudaFree(d_duplicate_flags);
    cudaFree(d_final_flags);

    return cudaGetLastError();
}

// Paired-end specific filtering
cudaError_t filter_paired_alignments(
    AlignmentResult* d_r1_results,
    AlignmentResult* d_r2_results,
    uint32_t num_pairs,
    const FilterParams& params,
    uint32_t& out_num_kept,
    cudaStream_t stream
) {
    // Similar to above, but uses validate_pairs() as well
    // ...
}

} // namespace cuda
} // namespace winalign
```

### CMakeLists.txt Update

```cmake
# In src/cuda/CMakeLists.txt
add_library(winalign_cuda OBJECT
    # ... other files ...
    filtering.cu
    filtering_quality.cu
    filtering_duplicates.cu
    filtering_pairs.cu
    # Note: filtering_device.cuh is header-only
)
```

---

## 🎯 File 2: alignment.cu (566 → 400 lines)

### Current Structure

```
Lines   1-50   : Includes, device helper functions (match_score, max3)
Lines  51-250  : smith_waterman_banded_warp_kernel (Phase 3 implementation)
Lines 251-350  : smith_waterman_kernel (legacy thread-per-seed)
Lines 351-450  : Host wrappers (smith_waterman_align, etc.)
Lines 451-566  : MAPQ calculation, CIGAR generation
```

### New Structure

```
src/cuda/
├── alignment.cu                    (200 lines) - Main API + host wrappers
├── alignment_banded_warp.cu        (200 lines) - Warp-optimized kernel (Phase 3)
├── alignment_legacy.cu             (150 lines) - Fallback kernel
└── alignment_device.cuh            (100 lines) - Device helpers
```

### Extraction Details

**alignment_device.cuh** (100 lines) - Shared Device Functions
```cuda
#pragma once

namespace winalign {
namespace cuda {
namespace alignment_internal {

// Match/mismatch scoring
__device__ inline int32_t match_score(char a, char b, const SWParams& params) {
    if (a == b || a == 'N' || b == 'N') {
        return params.match_score;
    }
    return params.mismatch_score;
}

// Max of three values
__device__ inline int32_t max3(int32_t a, int32_t b, int32_t c) {
    return max(max(a, b), c);
}

// MAPQ calculation helper
__device__ inline uint8_t calculate_mapq(int32_t score, int32_t best_score, int32_t second_best) {
    // MAPQ calculation logic
    // ...
}

// CIGAR operation encoding
__device__ inline uint32_t encode_cigar_op(char op, uint32_t count) {
    // CIGAR encoding
    // ...
}

} // namespace alignment_internal
} // namespace cuda
} // namespace winalign
```

**alignment_banded_warp.cu** (200 lines) - Phase 3 Implementation
```cuda
#include "winalign/cuda/alignment.cuh"
#include "alignment_device.cuh"

namespace winalign {
namespace cuda {

// Warp-optimized banded SW kernel (from Phase 3)
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
    uint32_t band_width
) {
    // Implementation from current alignment.cu lines 25-208
    // Uses alignment_internal::match_score() and max3()
    // ...
}

// Host wrapper
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
    // Launch configuration
    const uint32_t WARP_SIZE = 32;
    const uint32_t WARPS_PER_BLOCK = 4;
    const uint32_t block_size = WARP_SIZE * WARPS_PER_BLOCK;
    const uint32_t num_warps = (num_seeds + WARP_SIZE - 1) / WARP_SIZE;
    const uint32_t grid_size = (num_warps + WARPS_PER_BLOCK - 1) / WARPS_PER_BLOCK;

    const uint32_t shared_mem_per_warp = (band_width * 2 + 1) * 2 * sizeof(int32_t);
    const uint32_t shared_mem_total = shared_mem_per_warp * WARPS_PER_BLOCK;

    smith_waterman_banded_warp_kernel<<<grid_size, block_size, shared_mem_total, stream>>>(
        reads.sequences, reads.offsets, reads.lengths,
        seeds, num_seeds, reference, ref_length,
        params, results, band_width
    );

    return cudaGetLastError();
}

} // namespace cuda
} // namespace winalign
```

**alignment_legacy.cu** (150 lines) - Fallback Kernel
```cuda
#include "winalign/cuda/alignment.cuh"
#include "alignment_device.cuh"

namespace winalign {
namespace cuda {

// Legacy thread-per-seed kernel (fallback for very long reads or limited shared memory)
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
    // Implementation from current alignment.cu lines 251-350
    // Each thread handles one seed independently
    // ...
}

// Host wrapper
cudaError_t smith_waterman_align_legacy(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    const SWParams& params,
    AlignmentResult* results,
    cudaStream_t stream
) {
    // Simple launch configuration
    dim3 block(256);
    dim3 grid((num_seeds + block.x - 1) / block.x);

    smith_waterman_kernel<<<grid, block, 0, stream>>>(
        reads.sequences, reads.offsets, reads.lengths,
        seeds, num_seeds, reference, ref_length,
        params, results
    );

    return cudaGetLastError();
}

} // namespace cuda
} // namespace winalign
```

**alignment.cu** (200 lines) - Main API
```cuda
#include "winalign/cuda/alignment.cuh"

namespace winalign {
namespace cuda {

// Forward declarations
extern cudaError_t smith_waterman_align_banded_warp(/* ... */);
extern cudaError_t smith_waterman_align_legacy(/* ... */);

// Main alignment function - auto-selects best kernel
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
    if (num_seeds == 0) {
        return cudaSuccess;
    }

    // Auto-select kernel based on read length and shared memory availability
    uint32_t max_read_len = 0;
    cudaMemcpy(&max_read_len, &reads.lengths[0], sizeof(uint32_t),
               cudaMemcpyDeviceToHost);

    // Use banded warp kernel if reads are reasonably sized
    const uint32_t MAX_READ_FOR_BANDED = 512;
    const uint32_t BAND_WIDTH = 64;

    if (max_read_len <= MAX_READ_FOR_BANDED) {
        // Check if we have enough shared memory
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, 0);

        const uint32_t shared_mem_needed = (BAND_WIDTH * 2 + 1) * 2 * sizeof(int32_t) * 4; // 4 warps
        if (shared_mem_needed <= prop.sharedMemPerBlock) {
            // Use optimized banded warp kernel
            return smith_waterman_align_banded_warp(
                reads, seeds, num_seeds, reference, ref_length,
                params, results, BAND_WIDTH, stream
            );
        }
    }

    // Fallback to legacy kernel
    return smith_waterman_align_legacy(
        reads, seeds, num_seeds, reference, ref_length,
        params, results, stream
    );
}

} // namespace cuda
} // namespace winalign
```

---

## 🎯 File 3: seeding.cu (565 → 400 lines)

### Current Structure

```
Lines   1-50   : Includes, device helpers (char_to_2bit, char_to_base)
Lines  51-150  : occ_rank (basic version)
Lines 151-200  : occ_rank_cached (Phase 4 optimized)
Lines 201-300  : fm_seed_kernel_optimized (Phase 4)
Lines 301-400  : fm_seed_kernel (basic version)
Lines 401-500  : Host wrappers
Lines 501-565  : Compaction kernels
```

### New Structure

```
src/cuda/
├── seeding.cu                      (200 lines) - Main API + host wrappers
├── seeding_optimized.cu            (200 lines) - Phase 4 optimized kernels
├── seeding_legacy.cu               (150 lines) - Basic seeding kernels
└── seeding_device.cuh              (100 lines) - Device helpers
```

### Extraction Details

**seeding_device.cuh** (100 lines)
```cuda
#pragma once

namespace winalign {
namespace cuda {
namespace seeding_internal {

// Base encoding
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
        default: return 4;  // N or invalid
    }
}

// Backward search helper (shared between optimized and legacy)
__device__ inline bool backward_search_step(
    const FMIndex& fm_index,
    uint8_t base,
    uint64_t& sp,
    uint64_t& ep,
    uint64_t (*occ_rank_func)(const FMIndex&, uint8_t, int64_t)
) {
    if (base >= 4) return false;

    uint64_t occ_sp = (sp == 0) ? 0 : occ_rank_func(fm_index, base, sp - 1);
    uint64_t occ_ep = occ_rank_func(fm_index, base, ep);

    sp = fm_index.c_table[base] + occ_sp;
    ep = fm_index.c_table[base] + occ_ep - 1;

    return sp <= ep;
}

} // namespace seeding_internal
} // namespace cuda
} // namespace winalign
```

**seeding_optimized.cu** (200 lines) - Phase 4
```cuda
#include "winalign/cuda/seeding.cuh"
#include "seeding_device.cuh"

namespace winalign {
namespace cuda {

// Optimized occ_rank with shared memory caching
__device__ uint64_t occ_rank_cached(
    const FMIndex& fm_index,
    const uint8_t* shared_bwt,
    uint64_t cache_start,
    uint64_t cache_size,
    uint8_t base,
    int64_t pos
) {
    // Implementation from current seeding.cu lines 165-199
    // ...
}

// Optimized FM seeding kernel with BWT caching and filtering
__global__ void fm_seed_kernel_optimized(
    const ReadBatch reads,
    const FMIndex fm_index,
    Seed* tmp_seeds,
    uint32_t max_seeds_per_read,
    uint32_t kmer_size,
    uint32_t step,
    uint32_t max_hits_per_seed,
    uint32_t min_hit_threshold,
    uint32_t* seed_counts
) {
    // Implementation from current seeding.cu lines 202-302
    // Uses shared memory for BWT caching
    // Filters repetitive seeds
    // ...
}

// Host wrapper
cudaError_t generate_gpu_seeds_optimized(
    const ReadBatch& reads,
    const FMIndex& fm_index,
    Seed* seeds,
    uint32_t max_seeds_per_read,
    uint32_t kmer_size,
    uint32_t step,
    uint32_t& out_total_seeds,
    cudaStream_t stream
) {
    // Implementation from current seeding.cu lines 306-425
    // ...
}

} // namespace cuda
} // namespace winalign
```

**seeding_legacy.cu** (150 lines)
```cuda
#include "winalign/cuda/seeding.cuh"
#include "seeding_device.cuh"

namespace winalign {
namespace cuda {

// Basic occ_rank (no caching)
__device__ uint64_t occ_rank(
    const FMIndex& fm_index,
    uint8_t base,
    int64_t pos
) {
    // Implementation from current seeding.cu lines 28-51
    // ...
}

// Basic FM seeding kernel
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
    // Implementation from current seeding.cu lines 87-139
    // ...
}

// Compaction kernel
__global__ void compact_seeds_kernel(/* ... */) {
    // Implementation from current seeding.cu lines 141-160
    // ...
}

} // namespace cuda
} // namespace winalign
```

**seeding.cu** (200 lines) - Main API
```cuda
#include "winalign/cuda/seeding.cuh"
#include <cub/cub.cuh>

namespace winalign {
namespace cuda {

// Forward declarations
extern cudaError_t generate_gpu_seeds_optimized(/* ... */);

// Main seeding function - uses optimized version by default
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
    // Auto-use optimized version (from Phase 4)
    return generate_gpu_seeds_optimized(
        reads, fm_index, seeds, max_seeds_per_read,
        kmer_size, step, out_total_seeds, stream
    );
}

} // namespace cuda
} // namespace winalign
```

---

## 🔄 Refactoring Process

### Week 2, Day 1-2: filtering.cu

1. Create `filtering_device.cuh` with shared helpers
2. Extract quality filtering to `filtering_quality.cu`
3. Extract duplicate marking to `filtering_duplicates.cu`
4. Extract paired-end validation to `filtering_pairs.cu`
5. Update `filtering.cu` to be thin wrapper
6. Update CMakeLists.txt
7. Build and test

### Week 2, Day 3-4: alignment.cu

1. Create `alignment_device.cuh` with shared helpers
2. Extract banded warp kernel to `alignment_banded_warp.cu`
3. Extract legacy kernel to `alignment_legacy.cu`
4. Update `alignment.cu` to be auto-selector
5. Update CMakeLists.txt
6. Build and test

### Week 2, Day 5-6: seeding.cu

1. Create `seeding_device.cuh` with shared helpers
2. Extract optimized kernels to `seeding_optimized.cu`
3. Extract legacy kernels to `seeding_legacy.cu`
4. Update `seeding.cu` to use optimized by default
5. Update CMakeLists.txt
6. Build and test

### Week 2, Day 7: Verification

```bash
# Check all files are under limits
find src/cuda -name "*.cu" -o -name "*.cuh" | xargs wc -l

# Run full test suite
cmake --build build --target all
./build/bin/winalign-test

# Run CUDA tests specifically
./build/bin/winalign-test --filter="*CUDA*"

# Check for memory leaks
cuda-memcheck ./build/bin/winalign-test
```

---

## ✅ Success Criteria

- [ ] All CUDA files under 500 lines
- [ ] No performance regression (±5%)
- [ ] All tests passing
- [ ] No CUDA errors or memory leaks
- [ ] Build time not significantly increased
- [ ] Documentation updated

---

**Timeline**: 7 days (2025-11-27 to 2025-12-03)
**Estimated Effort**: 25-30 hours
**Dependencies**: None (can start immediately)
**Priority**: 🟡 HIGH (after pipeline.cpp)
