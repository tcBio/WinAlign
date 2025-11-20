# filtering.cu Refactoring Plan

**Date**: 2025-11-19
**Status**: 📋 PLAN CREATED - Ready to execute
**Current Size**: 649 lines (30% over 500-line limit)
**Target**: 5 files, all under 500 lines

---

## 🎯 Objective

Split `filtering.cu` (649 lines) into 5 modular files following the same successful pattern used for pipeline.cpp refactoring.

---

## 📊 Current Structure Analysis

### Public API (from filtering.cuh)
```cpp
✅ uint32_t filter_by_quality(...)         // Quality filtering
✅ uint32_t mark_duplicates(...)           // Duplicate marking
✅ uint32_t validate_pairs(...)            // Paired-end validation
✅ cudaError_t sort_by_coordinate(...)     // Coordinate sorting
✅ uint32_t compact_results(...)           // Result compaction
✅ cudaError_t compute_statistics(...)     // Statistics computation
```

### Current Code Sections (649 lines)
```
Lines 1-25:    Includes & constants (SAM flags, structs)
Lines 26-52:   Device function: passes_quality_filter
Lines 55-66:   Kernel: filter_quality_kernel
Lines 69-92:   Kernel: mark_duplicates_kernel
Lines 95-130:  Kernel: validate_pairs_kernel
Lines 132-228: Kernel: compute_stats_kernel
Lines 238-323: Host: filter_by_quality (includes CUB reduction)
Lines 325-405: Host: mark_duplicates
Lines 406-490: Host: validate_pairs
Lines 491-552: Host: sort_by_coordinate (uses thrust::sort)
Lines 553-649: Host: compute_statistics
```

**Section Breakdown:**
- **Quality filtering**: 297 lines (device func + kernel + host with CUB)
- **Duplicate marking**: 80 lines (kernel + host)
- **Pair validation**: 85 lines (kernel + host)
- **Sorting**: 62 lines (thrust sort wrapper)
- **Statistics**: 97 lines (kernel + host)
- **Common**: 25 lines (includes, constants, predicates)

---

## 🔧 Extraction Plan (5 Files)

### File 1: filtering_device.cuh (~100 lines)
**Purpose**: Shared constants, device functions, and predicates

**Contents:**
```cpp
- SAM flag constants (SAM_FLAG_SECONDARY, etc.)
- Device function: passes_quality_filter(result, params)
- Functor: IsFiltered (used in thrust::remove_if)
- Functor: CoordinateComparator (used in thrust::sort)
- Any other shared device utilities
```

**Estimated Lines**: ~100
**Status**: Header file (no 500-line limit, but keep under 300)

---

### File 2: filtering_quality.cu (~180 lines)
**Purpose**: Quality filtering kernel and host function

**Contents:**
```cpp
#include "filtering_device.cuh"
#include <cuda_runtime.h>
#include <cub/cub.cuh>

namespace winalign {
namespace cuda {

// Kernel: Quality filtering (lines 55-66, ~12 lines)
__global__ void filter_quality_kernel(
    AlignmentResult* results,
    uint32_t num_results,
    FilterParams params,
    uint32_t* filter_flags
);

// Host function: Filter by quality (lines 238-323, ~85 lines)
uint32_t filter_by_quality(
    AlignmentResult* results,
    uint32_t num_results,
    const FilterParams& params,
    cudaStream_t stream
) {
    // Allocate filter flags
    // Launch kernel
    // CUB reduction to count passing alignments
    // Cleanup and return count
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~180 (includes + kernel + host + CUB logic)
**Target**: <200 lines ✅

---

### File 3: filtering_duplicates.cu (~150 lines)
**Purpose**: Duplicate marking and result compaction

**Contents:**
```cpp
#include "filtering_device.cuh"
#include <cuda_runtime.h>
#include <thrust/remove.h>

namespace winalign {
namespace cuda {

// Kernel: Mark duplicates (lines 69-92, ~24 lines)
__global__ void mark_duplicates_kernel(
    AlignmentResult* results,
    uint32_t num_results,
    uint32_t* duplicate_flags
);

// Host function: Mark duplicates (lines 325-405, ~80 lines)
uint32_t mark_duplicates(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream
) {
    // Sort by position first (required for duplicate detection)
    // Launch kernel
    // Count duplicates
    // Return count
}

// Host function: Compact results (remove filtered)
uint32_t compact_results(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream
) {
    // Use thrust::remove_if with IsFiltered predicate
    // Return new size
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~150
**Target**: <200 lines ✅

---

### File 4: filtering_pairs.cu (~150 lines)
**Purpose**: Paired-end validation

**Contents:**
```cpp
#include "filtering_device.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Kernel: Validate pairs (lines 95-130, ~36 lines)
__global__ void validate_pairs_kernel(
    AlignmentResult* results1,
    AlignmentResult* results2,
    uint32_t num_pairs,
    uint32_t min_insert_size,
    uint32_t max_insert_size,
    uint32_t* pair_flags
);

// Host function: Validate pairs (lines 406-490, ~85 lines)
uint32_t validate_pairs(
    AlignmentResult* results1,
    AlignmentResult* results2,
    uint32_t num_pairs,
    uint32_t min_insert_size,
    uint32_t max_insert_size,
    cudaStream_t stream
) {
    // Allocate pair flags
    // Launch kernel
    // Count proper pairs
    // Set SAM_FLAG_PROPER_PAIR
    // Return count
}

} // namespace cuda
} // namespace winalign
```

**Estimated Lines**: ~150
**Target**: <200 lines ✅

---

### File 5: filtering.cu (main, ~200 lines)
**Purpose**: Main API, sorting, and statistics

**Contents:**
```cpp
#include "winalign/cuda/filtering.cuh"
#include "filtering_device.cuh"
#include <cuda_runtime.h>
#include <thrust/sort.h>

namespace winalign {
namespace cuda {

// Kernel: Compute statistics (lines 132-228, ~97 lines)
__global__ void compute_stats_kernel(
    const AlignmentResult* results,
    uint32_t num_results,
    AlignmentStats* stats
);

// Host function: Sort by coordinate (lines 491-552, ~62 lines)
cudaError_t sort_by_coordinate(
    AlignmentResult* results,
    uint32_t num_results,
    cudaStream_t stream
) {
    // Use thrust::sort with CoordinateComparator
    thrust::device_ptr<AlignmentResult> d_ptr(results);
    thrust::sort(d_ptr, d_ptr + num_results, CoordinateComparator());
    return cudaGetLastError();
}

// Host function: Compute statistics (lines 553-649, ~97 lines)
cudaError_t compute_statistics(
    const AlignmentResult* results,
    uint32_t num_results,
    AlignmentStats* stats,
    cudaStream_t stream
) {
    // Launch compute_stats_kernel
    // Reduction operations for mean calculations
    // Copy stats back to host
    return cudaGetLastError();
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
| filtering_device.cuh | ~100 | 300 | 33% | ✅ |
| filtering_quality.cu | ~180 | 500 | 36% | ✅ |
| filtering_duplicates.cu | ~150 | 500 | 30% | ✅ |
| filtering_pairs.cu | ~150 | 500 | 30% | ✅ |
| filtering.cu | ~200 | 500 | 40% | ✅ |
| **TOTAL** | ~780 | - | - | - |

**Note**: Total is higher than original 649 because we're adding includes, namespace wrappers, and proper separation. Each file is well under the 500-line limit.

---

## 🔗 Dependencies

### External Dependencies
- `<cuda_runtime.h>` - CUDA runtime API
- `<thrust/sort.h>` - Thrust parallel sorting
- `<thrust/remove.h>` - Thrust parallel compaction
- `<cub/cub.cuh>` - CUB primitives (reduction)
- `"winalign/cuda/filtering.cuh"` - Public API header
- `"winalign/cuda/alignment.cuh"` - AlignmentResult definition

### Internal Dependencies
```
filtering_device.cuh (shared)
    ↓
├── filtering_quality.cu
├── filtering_duplicates.cu
├── filtering_pairs.cu
└── filtering.cu
```

All .cu files include filtering_device.cuh for shared utilities.

---

## 🎯 Integration Steps

### Step 1: Create filtering_device.cuh
```bash
# Extract shared code
- SAM flags
- passes_quality_filter device function
- IsFiltered functor
- CoordinateComparator functor
```

### Step 2: Create filtering_quality.cu
```bash
# Extract quality filtering
- filter_quality_kernel
- filter_by_quality host function (with CUB reduction)
```

### Step 3: Create filtering_duplicates.cu
```bash
# Extract duplicate marking
- mark_duplicates_kernel
- mark_duplicates host function
- compact_results host function
```

### Step 4: Create filtering_pairs.cu
```bash
# Extract pair validation
- validate_pairs_kernel
- validate_pairs host function
```

### Step 5: Update filtering.cu
```bash
# Keep main API functions
- compute_stats_kernel
- sort_by_coordinate
- compute_statistics
# Add include for filtering_device.cuh
```

### Step 6: Update CMakeLists.txt
```cmake
# Add new CUDA source files
cuda_add_library(winalign_cuda
    filtering.cu
    filtering_quality.cu
    filtering_duplicates.cu
    filtering_pairs.cu
    alignment.cu
    seeding.cu
    memory_manager.cu
)
```

### Step 7: Test
```bash
# Verify compilation
cd build && make winalign_cuda

# Run tests
./bin/winalign-test --gtest_filter="*Filtering*"
```

---

## ✅ Success Criteria

- [ ] All 5 files created
- [ ] All files under 500-line limit (quality.cu ~180, duplicates.cu ~150, pairs.cu ~150, filtering.cu ~200)
- [ ] filtering_device.cuh under 300 lines (~100)
- [ ] Code compiles without errors
- [ ] All tests pass
- [ ] Public API unchanged (filtering.cuh not modified)
- [ ] No performance regression

---

## 🚨 Risks & Mitigation

### Risk 1: CUB/Thrust Template Bloat
**Risk**: Thrust/CUB templates may increase line count
**Mitigation**: Keep template usage minimal, extract to device functions where possible

### Risk 2: Shared Code Duplication
**Risk**: Multiple files may need same helper functions
**Mitigation**: Use filtering_device.cuh for all shared code

### Risk 3: Build Configuration
**Risk**: CMake may have issues with multiple CUDA files
**Mitigation**: Update CMakeLists.txt carefully, test incrementally

---

## 📊 Comparison: Before vs After

### Before (Monolithic)
```
filtering.cu: 649 lines (30% over limit)
- Difficult to navigate
- All filtering logic mixed together
- Hard to test individual components
```

### After (Modular)
```
5 focused files: ~780 lines total (distributed)
- filtering_device.cuh:    ~100 lines (shared utilities)
- filtering_quality.cu:    ~180 lines (quality filtering)
- filtering_duplicates.cu: ~150 lines (duplicates)
- filtering_pairs.cu:      ~150 lines (pair validation)
- filtering.cu:            ~200 lines (sorting & stats)

Benefits:
✅ All files under limits
✅ Clear separation of concerns
✅ Easier to test
✅ Easier to maintain
✅ Parallel development possible
```

---

## 🎯 Next Steps

1. ✅ Analysis complete - This document
2. ⏳ Create filtering_device.cuh
3. ⏳ Create filtering_quality.cu
4. ⏳ Create filtering_duplicates.cu
5. ⏳ Create filtering_pairs.cu
6. ⏳ Update filtering.cu
7. ⏳ Update CMakeLists.txt
8. ⏳ Test compilation
9. ⏳ Validate functionality

**Estimated Time**: 3-4 hours
**Priority**: HIGH (Week 2, Days 8-10)

---

**Status**: 📋 **PLAN READY** - Ready to begin extraction
**Confidence**: HIGH (95%) - Pattern proven with pipeline.cpp
**Next Action**: Create filtering_device.cuh
