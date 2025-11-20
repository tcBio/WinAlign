# seeding.cu Refactoring Plan

**Date**: 2025-11-19
**Status**: 📋 PLAN CREATED - Ready to execute
**Current Size**: 565 lines (13% over 500-line limit)
**Target**: 4 files, all under 500 lines

---

## 🎯 Objective

Split `seeding.cu` (565 lines) into 4 modular files, separating Phase 4 optimized code from basic FM-index operations.

---

## 📊 Current Structure Analysis

### Public API (from seeding.cuh)
```cpp
✅ cudaError_t generate_gpu_seeds_optimized(...)  // Phase 4 with shared mem BWT
✅ cudaError_t generate_gpu_seeds(...)             // Auto-select best kernel
✅ cudaError_t allocate_read_batch(...)            // Memory management
✅ cudaError_t free_read_batch(...)                // Memory management
✅ cudaError_t allocate_fm_index(...)              // Memory management
✅ cudaError_t free_fm_index(...)                  // Memory management
✅ cudaError_t copy_fm_index_to_device(...)        // Data transfer
```

### Current Code Sections (565 lines)
```
Lines 1-7:     Includes
Lines 8-27:    Device helpers (char_to_2bit, char_to_base)
Lines 28-85:   Device function: occ_rank (basic)
Lines 53-139:  Device function: backward_search
Lines 87-140:  Kernel: fm_seed_kernel (basic)
Lines 141-161: Kernel: compact_seeds_kernel
Lines 165-200: Device function: occ_rank_cached (Phase 4 optimized)
Lines 202-303: Kernel: fm_seed_kernel_optimized (Phase 4)
Lines 306-428: Host: generate_gpu_seeds_optimized (Phase 4)
Lines 429-445: Host: generate_gpu_seeds (wrapper)
Lines 447-565: Host: Memory management (allocate/free/copy)
```

**Section Breakdown:**
- **Phase 4 optimized**: 225 lines (occ_rank_cached + kernel + host)
- **Basic FM operations**: 139 lines (basic occ_rank + backward_search + kernel)
- **Compact seeds**: 21 lines
- **Main API & memory**: 157 lines
- **Device helpers**: 20 lines

---

## 🔧 Extraction Plan (4 Files)

### File 1: seeding_device.cuh (~150 lines)
**Purpose**: Shared device functions and FM-index operations

**Contents:**
```cpp
#pragma once
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Forward declarations
struct FMIndexView;
struct Seed;

// Device function: Convert DNA char to 2-bit encoding
__device__ inline uint8_t char_to_2bit(char c) {
    switch (c) {
        case 'A': case 'a': return 0;
        case 'C': case 'c': return 1;
        case 'G': case 'g': return 2;
        case 'T': case 't': return 3;
        default: return 0; // Treat N as A
    }
}

// Device function: Convert DNA char to base index
__device__ inline uint8_t char_to_base(char c) {
    // Similar to char_to_2bit
}

// Device function: Basic occ_rank (lines 28-51, ~24 lines)
__device__ uint64_t occ_rank(
    const FMIndexView& fm_index,
    uint8_t base,
    uint64_t k
) {
    // Count occurrences of base in BWT[0..k]
    // Using checkpoint tables
    // Basic implementation (not cached)
}

// Device function: Backward search (lines 53-139, ~87 lines)
__device__ bool backward_search(
    const FMIndexView& fm_index,
    const char* query,
    uint32_t query_len,
    uint64_t& sp,
    uint64_t& ep
) {
    // FM-index backward search
    // Returns search interval [sp, ep)
    // ~87 lines of FM-index logic
}

// Constants
constexpr uint32_t MAX_SEED_LENGTH = 32;
constexpr uint32_t MIN_SEED_LENGTH = 15;

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~150
**Status**: Header file (target <300 lines) ✅

---

### File 2: seeding_basic.cu (~180 lines)
**Purpose**: Basic FM-index seeding kernel (non-optimized)

**Contents:**
```cpp
#include "seeding_device.cuh"
#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Kernel: Basic FM seeding (lines 87-140, ~54 lines)
__global__ void fm_seed_kernel(
    const char* sequences,
    const uint32_t* offsets,
    const uint32_t* lengths,
    uint32_t num_reads,
    FMIndexView fm_index,
    Seed* seeds,
    uint32_t* num_seeds_per_read,
    uint32_t max_seeds_per_read,
    uint32_t kmer_size
) {
    // One thread per read
    // Extract k-mers from read
    // Perform backward_search for each k-mer
    // Store seeds
    // Basic implementation without shared memory optimization
}

// Kernel: Compact seeds (lines 141-161, ~21 lines)
__global__ void compact_seeds_kernel(
    const Seed* seeds_in,
    Seed* seeds_out,
    const uint32_t* num_seeds_per_read,
    uint32_t num_reads,
    uint32_t* scan_offsets
) {
    // Compact seeds from sparse array to dense array
    // Remove empty slots
}

// Host function: Generate seeds using basic kernel
cudaError_t generate_gpu_seeds_basic(
    const ReadBatch& reads,
    const FMIndexView* d_fm_index,
    Seed* d_seeds,
    uint32_t* d_num_seeds,
    uint32_t kmer_size,
    cudaStream_t stream
) {
    // Calculate grid/block dimensions
    // Launch fm_seed_kernel
    // Launch compact_seeds_kernel
    // Return result
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~180
**Target**: <250 lines ✅

---

### File 3: seeding_optimized.cu (~250 lines)
**Purpose**: Phase 4 optimized seeding with shared memory BWT caching

**Contents:**
```cpp
#include "seeding_device.cuh"
#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// ===== PHASE 4: Optimized Seeding with Shared Memory BWT Cache =====

// Device function: Optimized occ_rank with shared mem (lines 165-200, ~36 lines)
__device__ uint64_t occ_rank_cached(
    const FMIndexView& fm_index,
    uint8_t base,
    uint64_t k,
    const uint8_t* shared_bwt,  // Shared memory cache
    uint32_t cache_offset,
    uint32_t cache_size
) {
    // Check if k is in cached region
    // Use shared memory for fast access if available
    // Fall back to global memory otherwise
    // Significantly faster than basic occ_rank
}

// Kernel: Phase 4 optimized FM seeding (lines 202-303, ~102 lines)
__global__ void fm_seed_kernel_optimized(
    const char* sequences,
    const uint32_t* offsets,
    const uint32_t* lengths,
    uint32_t num_reads,
    FMIndexView fm_index,
    Seed* seeds,
    uint32_t* num_seeds_per_read,
    uint32_t max_seeds_per_read,
    uint32_t kmer_size
) {
    // Shared memory for BWT caching
    extern __shared__ uint8_t shared_bwt[];

    // Load BWT chunks into shared memory cooperatively
    // One thread per read
    // Extract k-mers from read
    // Perform backward_search using occ_rank_cached
    // 3-5x faster than basic kernel due to shared memory
}

// Host function: Phase 4 optimized (lines 306-428, ~123 lines)
cudaError_t generate_gpu_seeds_optimized(
    const ReadBatch& reads,
    const FMIndexView* d_fm_index,
    Seed* d_seeds,
    uint32_t* d_num_seeds,
    uint32_t kmer_size,
    cudaStream_t stream
) {
    // Calculate optimal shared memory size
    // Determine BWT cache size based on SM capacity
    // Calculate grid/block dimensions
    // Launch fm_seed_kernel_optimized with shared memory
    // Launch compact_seeds_kernel
    // Return result
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~250
**Target**: <300 lines ✅

---

### File 4: seeding.cu (main, ~180 lines)
**Purpose**: Main API, auto-selection, memory management

**Contents:**
```cpp
#include "winalign/cuda/seeding.cuh"
#include "seeding_device.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Forward declare kernels
extern "C" __global__ void fm_seed_kernel_optimized(...);
extern "C" __global__ void compact_seeds_kernel(...);

// Forward declare optimized host function
cudaError_t generate_gpu_seeds_optimized(...);

// Host function: Auto-select best kernel (lines 429-445, ~16 lines)
cudaError_t generate_gpu_seeds(
    const ReadBatch& reads,
    const FMIndexView* d_fm_index,
    Seed* d_seeds,
    uint32_t* d_num_seeds,
    uint32_t kmer_size,
    cudaStream_t stream
) {
    // Check GPU capabilities
    // Always use optimized version (Phase 4)
    // Could add logic to fall back to basic kernel if shared mem limited
    return generate_gpu_seeds_optimized(
        reads, d_fm_index, d_seeds, d_num_seeds, kmer_size, stream
    );
}

// Host function: Allocate read batch (lines 447-466, ~19 lines)
cudaError_t allocate_read_batch(
    ReadBatch& batch,
    uint32_t max_reads,
    uint32_t max_read_length
) {
    // cudaMalloc for sequences, offsets, lengths
    // Initialize batch struct
}

// Host function: Free read batch (lines 467-474, ~7 lines)
cudaError_t free_read_batch(ReadBatch& batch) {
    // cudaFree all batch buffers
}

// Host function: Allocate FM-index (lines 475-500, ~25 lines)
cudaError_t allocate_fm_index(
    FMIndex& fm_index,
    uint64_t length,
    size_t occ_entries,
    size_t suffix_length
) {
    // cudaMalloc for BWT, C-table, occ table, suffix array
    // Initialize fm_index struct
}

// Host function: Free FM-index (lines 501-509, ~8 lines)
cudaError_t free_fm_index(FMIndex& fm_index) {
    // cudaFree all FM-index buffers
}

// Host function: Copy FM-index to device (lines 510-565, ~55 lines)
cudaError_t copy_fm_index_to_device(
    FMIndex& dst,
    const HostFMIndexView& src,
    cudaStream_t stream
) {
    // cudaMemcpyAsync for all FM-index components
    // BWT, C-table, occurrence table, suffix array
    // Use pinned memory for faster transfers
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~180
**Target**: <250 lines ✅

---

## 📋 File Size Summary

| File | Estimated Lines | Limit | Usage | Status |
|------|----------------|-------|-------|--------|
| seeding_device.cuh | ~150 | 300 | 50% | ✅ |
| seeding_basic.cu | ~180 | 500 | 36% | ✅ |
| seeding_optimized.cu | ~250 | 500 | 50% | ✅ |
| seeding.cu | ~180 | 500 | 36% | ✅ |
| **TOTAL** | ~760 | - | - | - |

**Note**: Total is higher than original 565 due to file separation overhead. Each file is well under limit.

---

## 🔗 Dependencies

### External Dependencies
- `<cuda_runtime.h>` - CUDA runtime API
- `"winalign/cuda/seeding.cuh"` - Public API header

### Internal Dependencies
```
seeding_device.cuh (shared FM-index operations)
    ↓
├── seeding_basic.cu (basic kernel)
├── seeding_optimized.cu (Phase 4 with shared mem)
└── seeding.cu (main API)
```

---

## 🎯 Integration Steps

### Step 1: Create seeding_device.cuh
- Extract char_to_2bit, char_to_base
- Extract occ_rank (basic version)
- Extract backward_search
- Add shared constants

### Step 2: Create seeding_basic.cu
- Extract fm_seed_kernel (basic)
- Extract compact_seeds_kernel
- Add generate_gpu_seeds_basic host function
- Include seeding_device.cuh

### Step 3: Create seeding_optimized.cu
- Extract occ_rank_cached (Phase 4)
- Extract fm_seed_kernel_optimized
- Extract generate_gpu_seeds_optimized
- Include seeding_device.cuh

### Step 4: Update seeding.cu
- Keep main API wrapper (generate_gpu_seeds)
- Keep all memory management functions
- Add forward declarations for external kernels
- Include seeding_device.cuh

### Step 5: Update CMakeLists.txt
```cmake
cuda_add_library(winalign_cuda
    seeding.cu
    seeding_basic.cu
    seeding_optimized.cu
    # ... other CUDA files
)
```

### Step 6: Test
- Verify compilation
- Run seeding tests
- Benchmark Phase 4 optimization
- Verify memory management works

---

## ✅ Success Criteria

- [ ] All 4 files created
- [ ] All files under 500-line limit
- [ ] seeding_device.cuh under 300 lines
- [ ] Code compiles without errors
- [ ] All tests pass
- [ ] Public API unchanged
- [ ] Phase 4 optimization preserved
- [ ] No performance regression
- [ ] Memory management working

---

## 📊 Comparison: Before vs After

### Before (Monolithic)
```
seeding.cu: 565 lines (13% over limit)
- Phase 4 and basic mixed
- Hard to understand optimization
- Shared memory logic interleaved
```

### After (Modular)
```
4 focused files: ~760 lines total
- seeding_device.cuh:    ~150 lines (shared FM ops)
- seeding_basic.cu:      ~180 lines (basic kernel)
- seeding_optimized.cu:  ~250 lines (Phase 4)
- seeding.cu:            ~180 lines (main API)

Benefits:
✅ Clear separation of optimization levels
✅ Easy to compare basic vs optimized
✅ Shared FM-index operations reusable
✅ Memory management centralized
✅ All files under limits
```

---

**Status**: 📋 **PLAN READY** - Ready to execute
**Confidence**: HIGH (95%)
**Next Action**: Review all 3 plans, then execute systematically
