# alignment.cu Refactoring Plan

**Date**: 2025-11-19
**Status**: 📋 PLAN CREATED - Ready to execute
**Current Size**: 566 lines (13% over 500-line limit)
**Target**: 4 files, all under 500 lines

---

## 🎯 Objective

Split `alignment.cu` (566 lines) into 4 modular files, separating Phase 3 optimized code from legacy fallback code.

---

## 📊 Current Structure Analysis

### Public API (from alignment.cuh)
```cpp
✅ cudaError_t smith_waterman_align_banded_warp(...)  // Phase 3 optimized
✅ cudaError_t smith_waterman_align(...)               // Auto-select best kernel
✅ cudaError_t batch_smith_waterman(...)               // Batch processing
✅ cudaError_t generate_cigar(...)                     // CIGAR string generation
✅ cudaError_t calculate_mapping_quality(...)          // MAPQ calculation
✅ cudaError_t allocate_alignment_results(...)         // Memory management
✅ cudaError_t free_alignment_results(...)             // Memory management
```

### Current Code Sections (566 lines)
```
Lines 1-8:     Includes
Lines 9-20:    Device helpers (match_score, max3)
Lines 21-210:  Phase 3 warp-optimized banded kernel (~190 lines)
Lines 211-350: Legacy Smith-Waterman kernel (~140 lines)
Lines 351-391: Helper kernels (CIGAR, MAPQ) (~40 lines)
Lines 392-439: Phase 3 host function (~47 lines)
Lines 440-489: Auto-select host function (~49 lines)
Lines 490-566: Batch processing & memory (~76 lines)
```

**Section Breakdown:**
- **Phase 3 optimized**: 237 lines (kernel + host)
- **Legacy kernel**: 140 lines
- **Helper kernels**: 40 lines (CIGAR, MAPQ)
- **Main API**: 125 lines (auto-select, batch, memory)
- **Device helpers**: 20 lines

---

## 🔧 Extraction Plan (4 Files)

### File 1: alignment_device.cuh (~100 lines)
**Purpose**: Shared device functions and utilities

**Contents:**
```cpp
#pragma once
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Forward declarations
struct SWParams;
struct AlignmentResult;

// Device function: Match/mismatch scoring
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

// Device function: Compute diagonal position in band
__device__ inline int32_t band_index(int32_t i, int32_t j, int32_t band_width) {
    return (j - i) + band_width;  // Maps to [0, 2*band_width]
}

// Constants
constexpr int32_t NEG_INF = -1000000000;
constexpr uint32_t MAX_READ_LENGTH = 512;

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~100
**Status**: Header file (no 500-line limit, keep under 300)

---

### File 2: alignment_banded_warp.cu (~250 lines)
**Purpose**: Phase 3 warp-optimized banded Smith-Waterman

**Contents:**
```cpp
#include "alignment_device.cuh"
#include "winalign/cuda/alignment.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// ===== PHASE 3: Warp-Optimized Banded Smith-Waterman =====

// Kernel: Warp-optimized banded SW (lines 25-210, ~185 lines)
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
    // One warp (32 threads) per alignment
    // Uses shared memory for banded DP
    // Parallel computation within warp
    // Warp reduction for best score
    // ~185 lines of optimized DP logic
}

// Host function: Launch warp-optimized banded SW (lines 395-439, ~44 lines)
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
    // Calculate grid/block dimensions
    // Calculate shared memory requirements
    // Launch kernel
    // Check for errors
    // Return status
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~250 (kernel ~185 + host ~44 + includes ~20)
**Target**: <300 lines ✅

---

### File 3: alignment_legacy.cu (~200 lines)
**Purpose**: Original Smith-Waterman kernel (fallback for compatibility)

**Contents:**
```cpp
#include "alignment_device.cuh"
#include "winalign/cuda/alignment.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// ===== Original Smith-Waterman Kernel (Fallback) =====

// Kernel: Original SW - one thread per alignment (lines 213-350, ~137 lines)
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
    // One thread per alignment
    // Full DP matrix (no banding)
    // Simple implementation for compatibility
    // Used when banded kernel can't fit in shared memory
    // ~137 lines of standard DP
}

// Kernel: Generate CIGAR string (lines 352-370, ~18 lines)
__global__ void generate_cigar_kernel(
    const AlignmentResult* results,
    uint32_t num_results,
    char* cigar_strings,
    uint32_t* cigar_lengths
) {
    // Traceback through DP matrix
    // Generate CIGAR string
}

// Kernel: Calculate mapping quality (lines 372-391, ~19 lines)
__global__ void calculate_mapq_kernel(
    AlignmentResult* results,
    uint32_t num_results,
    uint8_t* mapq_values
) {
    // Calculate PHRED-scaled mapping quality
    // Based on alignment score and alternatives
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~200 (kernel ~137 + CIGAR ~18 + MAPQ ~19 + includes ~26)
**Target**: <250 lines ✅

---

### File 4: alignment.cu (main, ~200 lines)
**Purpose**: Main API, auto-selection, batch processing, memory management

**Contents:**
```cpp
#include "winalign/cuda/alignment.cuh"
#include "alignment_device.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Forward declare kernels from other files
extern "C" __global__ void smith_waterman_banded_warp_kernel(...);
extern "C" __global__ void smith_waterman_kernel(...);
extern "C" __global__ void generate_cigar_kernel(...);
extern "C" __global__ void calculate_mapq_kernel(...);

// Forward declare optimized host function
cudaError_t smith_waterman_align_banded_warp(...);

// Host function: Auto-select best kernel (lines 440-489, ~49 lines)
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
    // Check available shared memory
    // Calculate band width based on read lengths
    // Use banded warp kernel if shared memory is sufficient
    // Otherwise fall back to original kernel
    // Return result
}

// Host function: Batch processing (lines 490-509, ~19 lines)
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
    // Batch reads for optimal GPU utilization
    // Call smith_waterman_align
    // Handle overflow
}

// Host function: Generate CIGAR (lines 510-527, ~17 lines)
cudaError_t generate_cigar(
    const AlignmentResult* results,
    uint32_t num_results,
    char* cigar_strings,
    uint32_t* cigar_lengths,
    cudaStream_t stream
) {
    // Launch generate_cigar_kernel
    // Check errors
}

// Host function: Calculate MAPQ (lines 528-544, ~16 lines)
cudaError_t calculate_mapping_quality(
    AlignmentResult* results,
    uint32_t num_results,
    uint8_t* mapq_values,
    cudaStream_t stream
) {
    // Launch calculate_mapq_kernel
    // Check errors
}

// Host function: Allocate results (lines 545-556, ~11 lines)
cudaError_t allocate_alignment_results(
    AlignmentResult** results,
    uint32_t max_results
) {
    // cudaMalloc for results
    // Initialize
}

// Host function: Free results (lines 557-566, ~9 lines)
cudaError_t free_alignment_results(AlignmentResult* results) {
    // cudaFree
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~200
**Target**: <250 lines ✅

---

## 📋 File Size Summary

| File | Estimated Lines | Limit | Usage | Status |
|------|----------------|-------|-------|--------|
| alignment_device.cuh | ~100 | 300 | 33% | ✅ |
| alignment_banded_warp.cu | ~250 | 500 | 50% | ✅ |
| alignment_legacy.cu | ~200 | 500 | 40% | ✅ |
| alignment.cu | ~200 | 500 | 40% | ✅ |
| **TOTAL** | ~750 | - | - | - |

**Note**: Total is higher than original 566 due to file separation overhead (includes, namespace wrappers). Each file is well under limit.

---

## 🔗 Dependencies

### External Dependencies
- `<cuda_runtime.h>` - CUDA runtime API
- `"winalign/cuda/alignment.cuh"` - Public API header
- `"winalign/cuda/seeding.cuh"` - Seed structure

### Internal Dependencies
```
alignment_device.cuh (shared)
    ↓
├── alignment_banded_warp.cu (Phase 3 optimized)
├── alignment_legacy.cu (fallback kernels)
└── alignment.cu (main API)
```

### Kernel Declarations
`alignment.cu` needs forward declarations of kernels from other files since they're called via <<<>>> syntax.

---

## 🎯 Integration Steps

### Step 1: Create alignment_device.cuh
- Extract match_score, max3, band_index
- Add shared constants (NEG_INF, MAX_READ_LENGTH)
- Add helper inline device functions

### Step 2: Create alignment_banded_warp.cu
- Extract smith_waterman_banded_warp_kernel (Phase 3)
- Extract smith_waterman_align_banded_warp host function
- Include alignment_device.cuh

### Step 3: Create alignment_legacy.cu
- Extract smith_waterman_kernel (original)
- Extract generate_cigar_kernel
- Extract calculate_mapq_kernel
- Include alignment_device.cuh

### Step 4: Update alignment.cu
- Keep main API functions
- Add forward declarations for external kernels
- smith_waterman_align (auto-select)
- batch_smith_waterman
- generate_cigar, calculate_mapping_quality (host wrappers)
- allocate/free functions

### Step 5: Update CMakeLists.txt
```cmake
cuda_add_library(winalign_cuda
    alignment.cu
    alignment_banded_warp.cu
    alignment_legacy.cu
    # ... other CUDA files
)
```

### Step 6: Test
- Verify compilation
- Run alignment tests
- Benchmark Phase 3 vs legacy

---

## ✅ Success Criteria

- [ ] All 4 files created
- [ ] All files under 500-line limit
- [ ] alignment_device.cuh under 300 lines
- [ ] Code compiles without errors
- [ ] All tests pass
- [ ] Public API unchanged
- [ ] Phase 3 optimization still works
- [ ] Fallback to legacy kernel works
- [ ] No performance regression

---

## 📊 Comparison: Before vs After

### Before (Monolithic)
```
alignment.cu: 566 lines (13% over limit)
- Phase 3 and legacy mixed
- Hard to maintain both paths
- Difficult to optimize independently
```

### After (Modular)
```
4 focused files: ~750 lines total
- alignment_device.cuh:      ~100 lines (shared)
- alignment_banded_warp.cu:  ~250 lines (Phase 3)
- alignment_legacy.cu:       ~200 lines (fallback)
- alignment.cu:              ~200 lines (main API)

Benefits:
✅ Clear separation of optimization paths
✅ Independent development/testing
✅ Easy to add new kernel variants
✅ All files under limits
```

---

**Status**: 📋 **PLAN READY** - Ready to execute
**Confidence**: HIGH (95%)
**Next Action**: Create seeding.cu plan, then execute all 3
