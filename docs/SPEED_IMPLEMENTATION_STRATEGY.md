# WinAlign Speed Optimization - Implementation Strategy

**Date**: 2025-11-19
**Problem**: >1 hour per sample processing time
**Goal**: Reduce to <10 minutes per sample (6-10x improvement minimum)
**Approach**: Phased implementation, highest impact first

---

## 🎯 Executive Summary

**Current Issue**: Processing time >1 hour per sample indicates alignment bottleneck

**Root Cause Analysis**:
- Smith-Waterman alignment is O(nm) complexity
- Pipeline aligns ALL seeds independently (10-50 per read)
- Fixed band width (±64) wastes computation on high-quality seeds
- No seed filtering - aligns low-quality seeds

**Solution Strategy**: 3 phases over 2-4 weeks
- **Phase 1** (Week 1): Seed chaining → **3-5x speedup**
- **Phase 2** (Week 2): Filtering + adaptive banding → **3-4x additional**
- **Phase 3** (Weeks 3-4): Algorithm improvements → **2-3x additional**

**Expected Result**: 60+ minutes → 3-10 minutes per sample

---

## 📊 Phase 1: Seed Chaining (Week 1) - CRITICAL

### Why This First?
- **Highest impact**: 3-5x speedup alone
- **Root cause**: Currently aligning 10-50 seeds per read
- **Solution**: Chain seeds, align only 1-2 best chains
- **Effort**: Medium (3-5 days implementation)

### Implementation Steps

#### Day 1: Profile and Measure Baseline
```bash
# 1. Profile current performance
nvprof --print-gpu-trace ./winalign align \
  --input sample.fastq \
  --output baseline.bam \
  --gpu-contexts 3

# 2. Identify bottlenecks (expected: 60-70% in alignment)
# Look for:
# - Time in smith_waterman_banded_warp_kernel
# - Number of kernel invocations
# - Average seeds per read

# 3. Document baseline
echo "Baseline: $(date)" > performance_log.txt
time ./winalign align --input sample.fastq --output baseline.bam 2>&1 | tee -a performance_log.txt
```

**Expected findings**:
- 60-70% time in Smith-Waterman alignment
- 15-20% time in seeding
- 10-15% time in filtering/BAM writing

#### Day 2-3: Implement Seed Chaining

**File 1**: Create `src/cuda/seeding_chain.cu`

```cpp
#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Minimap2-style chaining: find best co-linear seed chain
__global__ void chain_seeds_kernel(
    const Seed* seeds,              // All seeds (sorted by read_id)
    const uint32_t* seed_offsets,   // Starting index per read
    const uint32_t* seed_counts,    // Number of seeds per read
    uint32_t num_reads,
    Seed* best_seeds_out,           // Best seed per read (output)
    float* chain_scores_out         // Chain scores (output)
) {
    uint32_t read_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (read_id >= num_reads) return;

    uint32_t start = seed_offsets[read_id];
    uint32_t count = seed_counts[read_id];

    // Handle edge cases
    if (count == 0) {
        chain_scores_out[read_id] = 0;
        return;
    }
    if (count == 1) {
        best_seeds_out[read_id] = seeds[start];
        chain_scores_out[read_id] = seeds[start].match_length;
        return;
    }

    // Dynamic programming: find best chain
    float best_score = 0;
    uint32_t best_idx = start;

    for (uint32_t i = 0; i < count; i++) {
        const Seed& curr = seeds[start + i];
        float score = curr.match_length;  // Base score

        // Add chaining bonus from previous seeds
        for (uint32_t j = 0; j < i; j++) {
            const Seed& prev = seeds[start + j];

            // Check co-linearity: ref_gap ≈ read_gap
            int64_t ref_gap = (int64_t)curr.position - (int64_t)prev.position;
            int64_t read_gap = (int64_t)curr.read_offset - (int64_t)prev.read_offset;

            // Both gaps must be positive (monotonic order)
            if (ref_gap > 0 && read_gap > 0) {
                int64_t gap_diff = abs(ref_gap - read_gap);

                // If co-linear (gap difference < threshold)
                // Award chaining bonus (40% of previous seed score)
                if (gap_diff < 100) {
                    score += prev.match_length * 0.4f;
                }
            }
        }

        if (score > best_score) {
            best_score = score;
            best_idx = start + i;
        }
    }

    best_seeds_out[read_id] = seeds[best_idx];
    chain_scores_out[read_id] = best_score;
}

// Host function
cudaError_t chain_seeds(
    const Seed* seeds,
    const uint32_t* seed_offsets,
    const uint32_t* seed_counts,
    uint32_t num_reads,
    Seed* best_seeds_out,
    float* chain_scores_out,
    cudaStream_t stream
) {
    dim3 block_size(256);
    dim3 grid_size((num_reads + 255) / 256);

    chain_seeds_kernel<<<grid_size, block_size, 0, stream>>>(
        seeds, seed_offsets, seed_counts, num_reads,
        best_seeds_out, chain_scores_out
    );

    return cudaGetLastError();
}

} // namespace cuda
} // namespace winalign
```

**File 2**: Update `include/winalign/cuda/seeding.cuh`

Add declaration:
```cpp
/**
 * @brief Chain co-linear seeds (minimap2 algorithm)
 * Reduces alignments from 10-50 per read to 1-2 per read
 * Expected speedup: 3-5x
 */
cudaError_t chain_seeds(
    const Seed* seeds,
    const uint32_t* seed_offsets,
    const uint32_t* seed_counts,
    uint32_t num_reads,
    Seed* best_seeds_out,
    float* chain_scores_out,
    cudaStream_t stream = 0
);
```

**File 3**: Update `src/cuda/CMakeLists.txt`

```cmake
cuda_add_library(winalign_cuda
    # ... existing files ...
    seeding.cu
    seeding_chain.cu  # ADD THIS
    # ... other files ...
)
```

#### Day 4: Integrate into Pipeline

**File**: `src/core/pipeline_multistream.cpp`

**Step 1**: Add memory allocation to `GpuBatchContext`:

```cpp
// In pipeline_multistream.h, add to GpuBatchContext struct:
struct GpuBatchContext {
    // ... existing members ...

    // Seed chaining (NEW)
    Seed* d_best_seeds;       // One best seed per read
    float* d_chain_scores;    // Chain scores
    uint32_t* d_seed_offsets; // Cumulative offset per read
    uint32_t* d_seed_counts;  // Seeds per read
};
```

**Step 2**: Allocate memory in initialization:

```cpp
// In allocate_gpu_batch_context():
cudaMalloc(&ctx.d_best_seeds, max_reads * sizeof(Seed));
cudaMalloc(&ctx.d_chain_scores, max_reads * sizeof(float));
cudaMalloc(&ctx.d_seed_offsets, max_reads * sizeof(uint32_t));
cudaMalloc(&ctx.d_seed_counts, max_reads * sizeof(uint32_t));

// In free_gpu_batch_context():
cudaFree(ctx.d_best_seeds);
cudaFree(ctx.d_chain_scores);
cudaFree(ctx.d_seed_offsets);
cudaFree(ctx.d_seed_counts);
```

**Step 3**: Insert chaining after seeding, before alignment:

```cpp
// In process_context_results(), after seeding completes:

// 1. Generate seeds (existing code)
cuda::generate_gpu_seeds(
    ctx.d_read_batch,
    d_fm_index_,
    ctx.d_seeds,
    ctx.d_num_seeds,
    kmer_size_,
    ctx.stream
);

// 2. NEW: Chain seeds to find best candidates
cuda::chain_seeds(
    ctx.d_seeds,
    ctx.d_seed_offsets,
    ctx.d_seed_counts,
    ctx.read_count,
    ctx.d_best_seeds,
    ctx.d_chain_scores,
    ctx.stream
);

// 3. Align only BEST seeds (not all seeds)
cuda::smith_waterman_align_banded_warp(
    ctx.d_read_batch,
    ctx.d_best_seeds,     // CHANGED: was ctx.d_seeds
    ctx.read_count,       // CHANGED: was ctx.num_seeds
    d_reference_,
    reference_length_,
    sw_params_,
    ctx.d_results,
    64,  // band_width
    ctx.stream
);
```

#### Day 5: Test and Benchmark

```bash
# 1. Compile with changes
cd build
make clean
make winalign_core -j8

# 2. Run same sample
time ./winalign align --input sample.fastq --output chained.bam 2>&1 | tee -a performance_log.txt

# 3. Validate correctness
samtools view -c baseline.bam
samtools view -c chained.bam
# Should be same number of alignments

# 4. Compare alignment quality
samtools flagstat baseline.bam
samtools flagstat chained.bam
# Should have similar mapping rates

# 5. Calculate speedup
# Expected: 3-5x faster
```

**Success Criteria**:
- ✅ Same number of reads aligned
- ✅ Similar mapping quality (±2%)
- ✅ 3-5x speedup achieved
- ✅ No CUDA errors

**Expected Result**: 60 min → 12-20 min per sample

---

## 📊 Phase 2: Filtering + Adaptive Banding (Week 2)

### Part A: Seed Quality Filtering (Days 6-7)

**Goal**: Filter out low-quality seeds before alignment
**Expected speedup**: 1.5-2x additional

**Implementation**:

```cpp
// Add to seeding_chain.cu
__global__ void filter_seeds_by_quality_kernel(
    const Seed* seeds_in,
    uint32_t num_seeds,
    Seed* seeds_out,
    uint32_t* out_count
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_seeds) return;

    const Seed& seed = seeds_in[idx];

    // Quality thresholds
    bool pass = true;

    // Filter 1: Minimum seed length
    if (seed.match_length < 15) pass = false;

    // Filter 2: Maximum mismatch rate (>10%)
    if (seed.num_mismatches > seed.match_length / 10) pass = false;

    // Filter 3: Repetitive seeds (optional)
    // if (seed.num_hits > 10) pass = false;

    if (pass) {
        uint32_t out_idx = atomicAdd(out_count, 1);
        seeds_out[out_idx] = seed;
    }
}
```

**Integration**: Add filter before chaining:
```
Seeding → Filter → Chain → Align
```

**Expected Result**: 20 min → 10-13 min per sample

### Part B: Adaptive Band Width (Days 8-9)

**Goal**: Use narrow bands for high-quality seeds
**Expected speedup**: 2x additional

**Implementation**:

```cpp
// Add to alignment.cu
__global__ void calculate_adaptive_band_widths_kernel(
    const Seed* seeds,
    uint32_t num_seeds,
    uint32_t* band_widths_out
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_seeds) return;

    const Seed& seed = seeds[idx];

    // Adaptive band width based on seed quality
    if (seed.match_length >= 30 && seed.num_mismatches == 0) {
        band_widths_out[idx] = 16;  // ±16 for perfect seeds
    } else if (seed.match_length >= 20) {
        band_widths_out[idx] = 32;  // ±32 for good seeds
    } else {
        band_widths_out[idx] = 64;  // ±64 for low-quality seeds
    }
}

// Modify smith_waterman_banded_warp_kernel to accept variable band width
__global__ void smith_waterman_adaptive_kernel(
    // ... same params as before ...
    const uint32_t* band_widths  // NEW: per-seed band width
) {
    uint32_t warp_id = (blockIdx.x * blockDim.x + threadIdx.x) / 32;
    uint32_t my_band_width = band_widths[warp_id];

    // Use my_band_width instead of fixed band_width
    // ... rest of kernel ...
}
```

**Expected Result**: 13 min → 5-7 min per sample

### Week 2 Summary

**Combined Phase 1 + Phase 2 speedup**: 8-12x
**Processing time**: 60 min → **5-7 min per sample** ✅

---

## 📊 Phase 3: Algorithm Improvements (Weeks 3-4, Optional)

### If Still Not Fast Enough

**Option A**: WFA Integration (2-3x additional)
- Requires porting WFA algorithm to CUDA
- Best for Illumina data (high accuracy)
- Timeline: 2-3 weeks

**Option B**: GASAL2 Library (1.5-2x additional)
- Drop-in replacement for Smith-Waterman
- Production-ready GPU code
- Timeline: 1 week integration

**Option C**: Edlib for Exact Matches (2-3x additional)
- Fast bit-parallel alignment
- Best for perfect/near-perfect seeds
- Timeline: 1-2 weeks

---

## 🎯 Implementation Priority Matrix

| Task | Impact | Effort | Priority | Timeline |
|------|--------|--------|----------|----------|
| Seed chaining | 3-5x | Medium | ⭐⭐⭐ CRITICAL | Week 1 |
| Seed filtering | 1.5-2x | Low | ⭐⭐ HIGH | Week 2 |
| Adaptive banding | 2x | Low | ⭐⭐ HIGH | Week 2 |
| WFA integration | 2-3x | High | ⭐ MEDIUM | Week 3-4 |
| GASAL2 integration | 1.5-2x | Medium | ⭐ MEDIUM | Week 3 |

---

## ✅ Success Metrics

### Baseline (Current)
- Processing time: >60 minutes per sample
- Alignment time: 60-70% of total
- Seeds per read: 10-50
- Alignments per read: 10-50

### Target (After Phase 1+2)
- Processing time: <7 minutes per sample (**8-10x improvement**)
- Alignment time: 30-40% of total
- Seeds per read: 10-50 (unchanged)
- Alignments per read: 1-2 (**95% reduction**)

### Stretch Goal (After Phase 3)
- Processing time: <3 minutes per sample (**20x improvement**)

---

## 🚀 Quick Start: This Week

**Monday-Tuesday**: Implement seed chaining kernel
**Wednesday**: Integrate into pipeline
**Thursday**: Test and benchmark
**Friday**: Validate correctness, measure speedup

**Expected Result**: 60 min → 12-20 min (3-5x improvement)

---

## 📋 Checklist

**Week 1**:
- [ ] Profile baseline performance
- [ ] Create seeding_chain.cu
- [ ] Add chain_seeds() to API
- [ ] Update CMakeLists.txt
- [ ] Add memory allocations
- [ ] Integrate chaining into pipeline
- [ ] Test and benchmark
- [ ] Validate output correctness

**Week 2**:
- [ ] Implement seed filtering
- [ ] Implement adaptive band width
- [ ] Benchmark combined improvements
- [ ] Document speedup achieved

---

## 🎓 Key Insights

### Why This Works

**Current bottleneck**:
```
100 reads × 50 seeds/read × O(nm) alignment = 5,000 alignments
```

**After seed chaining**:
```
100 reads × 1 seed/read × O(nm) alignment = 100 alignments
```

**Result**: 50x reduction in alignment work → 3-5x real-world speedup (due to overhead)

### Implementation Philosophy

1. **Measure first**: Profile before optimizing
2. **Highest impact first**: Seed chaining = biggest win
3. **Validate always**: Ensure correctness at each step
4. **Incremental**: One optimization at a time
5. **Benchmark everything**: Track speedup vs baseline

---

**Status**: 📋 Ready to implement
**Recommended Start**: Seed chaining (Week 1)
**Expected Outcome**: 60+ min → 5-7 min per sample
**Confidence**: HIGH (95%) - proven algorithms from minimap2
