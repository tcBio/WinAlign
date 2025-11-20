# WinAlign Speed Optimization Guide - Practical Steps

**Date**: 2025-11-19
**Focus**: Actionable improvements for alignment speed
**Target**: 2-10x speedup through algorithmic and implementation improvements

---

## 🎯 Executive Summary

**Current Implementation**:
- FM-Index seeding (Phase 4: shared memory BWT)
- Smith-Waterman alignment (Phase 3: warp-optimized banded)
- Multi-stream GPU pipeline

**Recommended Improvements** (Ordered by impact):
1. **Minimap2-style chaining** → 3-5x speedup (HIGH impact)
2. **Adaptive banding** → 2x speedup (HIGH impact)
3. **WFA (wavefront alignment)** → 2-3x speedup (MEDIUM-HIGH impact)
4. **Seed filtering/clustering** → 1.5-2x speedup (MEDIUM impact)
5. **GPU tensor cores** → 1.3-1.5x speedup (MEDIUM impact)

---

## 📊 Current Bottleneck Analysis

### Profiling Current Pipeline

**Expected time distribution**:
- Seeding (FM-index): ~15-20%
- Alignment (Smith-Waterman): ~60-70% ⚠️ **BOTTLENECK**
- Filtering: ~5-10%
- BAM writing: ~5-10%

**Smith-Waterman is the main bottleneck** because:
- O(nm) complexity (read length × reference window)
- Performs full DP even for high-quality seeds
- Processes all seeds independently
- Current band width (±64) may be too wide

---

## 🚀 Quick Wins (Phase 5) - Implement First

### 1. Adaptive Band Width (2x speedup)

**Current**: Fixed band width of ±64 for all alignments
**Problem**: Wastes computation on high-quality seeds

**Solution**: Adjust band width based on seed quality
```cpp
// In alignment.cu
__device__ uint32_t calculate_adaptive_band_width(const Seed& seed) {
    // High quality seed (exact match) → narrow band
    if (seed.match_length >= 30 && seed.num_mismatches == 0) {
        return 16;  // ±16 diagonal
    }
    // Medium quality → medium band
    else if (seed.match_length >= 20) {
        return 32;  // ±32 diagonal
    }
    // Low quality or short → wide band
    else {
        return 64;  // ±64 diagonal (current default)
    }
}

// Modify smith_waterman_banded_warp_kernel to accept per-seed band width
__global__ void smith_waterman_banded_warp_kernel_adaptive(
    const char* reads,
    const uint32_t* read_offsets,
    const uint32_t* read_lengths,
    const Seed* seeds,
    const uint32_t* band_widths,  // NEW: per-seed band width
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    SWParams params,
    AlignmentResult* results
) {
    // Use band_widths[warp_id] instead of fixed band_width
    uint32_t my_band_width = band_widths[warp_id];
    // ... rest of kernel
}
```

**Implementation steps**:
1. Add seed quality scoring in seeding kernel
2. Pre-compute band widths on GPU
3. Modify banded kernel to accept variable widths
4. Use dynamic shared memory allocation

**Expected speedup**: 2x (most seeds are high quality)

---

### 2. Seed Chaining (3-5x speedup) ⭐ **HIGHEST IMPACT**

**Current**: Aligns every seed independently
**Problem**: 10-50 seeds per read, all aligned individually

**Solution**: Chain co-linear seeds, align only best chain
```cpp
// Add new kernel: chain_seeds_kernel
__global__ void chain_seeds_kernel(
    const Seed* seeds,
    uint32_t num_seeds,
    const uint32_t* num_seeds_per_read,
    Seed* best_seeds_out,  // One best seed per read
    float* chain_scores    // Chaining scores
) {
    uint32_t read_id = blockIdx.x;

    // Find seeds for this read
    uint32_t start_idx = read_offsets[read_id];
    uint32_t end_idx = start_idx + num_seeds_per_read[read_id];

    if (end_idx - start_idx <= 1) {
        // Only one seed, use it
        best_seeds_out[read_id] = seeds[start_idx];
        return;
    }

    // Minimap2-style chaining:
    // Seeds are co-linear if: ref_pos[i+1] - ref_pos[i] ≈ read_pos[i+1] - read_pos[i]
    float best_chain_score = 0;
    uint32_t best_seed_idx = start_idx;

    for (uint32_t i = start_idx; i < end_idx; i++) {
        float chain_score = seeds[i].match_length;  // Start with seed score

        // Add bonus for seeds that chain with previous seeds
        for (uint32_t j = start_idx; j < i; j++) {
            int64_t ref_dist = seeds[i].position - seeds[j].position;
            int64_t read_dist = seeds[i].read_offset - seeds[j].read_offset;
            int64_t gap = abs(ref_dist - read_dist);

            // If co-linear (small gap), add chaining bonus
            if (gap < 500 && ref_dist > 0 && read_dist > 0) {
                chain_score += seeds[j].match_length * 0.5;  // Chaining bonus
            }
        }

        if (chain_score > best_chain_score) {
            best_chain_score = chain_score;
            best_seed_idx = i;
        }
    }

    best_seeds_out[read_id] = seeds[best_seed_idx];
    chain_scores[read_id] = best_chain_score;
}
```

**Pipeline changes**:
```
Current:  Seeding → Align ALL seeds → Pick best
New:      Seeding → Chain seeds → Align BEST chain only
```

**Implementation steps**:
1. Add seed chaining kernel after seeding
2. Reduce number of alignments from 10-50 per read to 1-2
3. Still align top 2 chains for paired-end validation

**Expected speedup**: 3-5x (align 95% fewer seeds)

---

### 3. Seed Filtering by Quality (1.5-2x speedup)

**Current**: Align all seeds regardless of quality
**Problem**: Many seeds are low quality or redundant

**Solution**: Filter seeds before alignment
```cpp
__device__ bool should_align_seed(const Seed& seed) {
    // Filter 1: Minimum seed length
    if (seed.match_length < 15) {
        return false;  // Too short, likely noise
    }

    // Filter 2: Maximum mismatches
    if (seed.num_mismatches > seed.match_length / 10) {
        return false;  // >10% mismatch rate
    }

    // Filter 3: Repetitive regions (optional)
    // if (seed.num_hits > 10) {
    //     return false;  // Seed maps to too many locations
    // }

    return true;
}
```

**Implementation steps**:
1. Add filtering pass after seeding
2. Use thrust::copy_if to compact seeds
3. Track filtered count for metrics

**Expected speedup**: 1.5-2x (filter 30-50% of low-quality seeds)

---

## 🧬 Algorithm Improvements (Medium-term)

### 4. WFA (Wave-Front Alignment) - Modern Alternative to SW

**What is WFA**:
- Recent algorithm (2020) from Santiago Marco-Sola et al.
- O(ns) complexity where s = edit distance (vs O(nm) for Smith-Waterman)
- 2-10x faster for high-identity alignments (>90% identity)

**Why it's faster**:
- Adaptive: only computes cells needed for alignment
- Bit-parallel: uses bit-vectors like Myers' algorithm
- Memory efficient: O(s) memory vs O(nm)

**When to use**:
- High quality seeds (exact match k-mers)
- Expected low edit distance (<5%)
- Illumina data (high accuracy)

**Implementation approach**:
```cpp
// Add WFA as alternative to Smith-Waterman
cudaError_t align_with_wfa(
    const ReadBatch& reads,
    const Seed* seeds,
    uint32_t num_seeds,
    const char* reference,
    uint64_t ref_length,
    AlignmentResult* results,
    cudaStream_t stream
) {
    // Use WFA for high-quality seeds
    // Fall back to banded SW for low-quality seeds
}
```

**References**:
- Paper: "Fast gap-affine pairwise alignment using the wavefront algorithm" (2020)
- Implementation: https://github.com/smarco/WFA2-lib
- GPU port: Possible but not yet available

**Expected speedup**: 2-3x for Illumina data

**Implementation timeline**: 1-2 months (requires porting to CUDA)

---

### 5. Minimap2-style Homopolymer Compressed Seeding

**What**: Compress homopolymers (AAAA → A) before seeding

**Why faster**:
- Reduces index size by ~20%
- More tolerant to homopolymer errors (common in Nanopore/PacBio)
- Faster seed lookups

**Implementation**:
```cpp
__device__ void compress_homopolymers(
    const char* read,
    uint32_t read_len,
    char* compressed_out,
    uint32_t* compressed_len_out,
    uint32_t* positions_map  // Maps compressed → original positions
) {
    uint32_t out_pos = 0;
    compressed_out[out_pos] = read[0];
    positions_map[out_pos] = 0;
    out_pos++;

    for (uint32_t i = 1; i < read_len; i++) {
        if (read[i] != read[i-1]) {  // Different from previous
            compressed_out[out_pos] = read[i];
            positions_map[out_pos] = i;
            out_pos++;
        }
    }
    *compressed_len_out = out_pos;
}
```

**Expected speedup**: 1.3-1.5x for seeding

---

## 🔬 Recent Innovations (Long-term)

### 6. GPU Tensor Cores for Alignment (2023-2024)

**What**: Use Tensor Cores (designed for matrix multiplication) for alignment

**How**:
- Convert alignment DP to matrix operations
- Use Tensor Cores (INT8 or FP16)
- Achieve 8-16x higher throughput than CUDA cores

**Challenges**:
- Requires NVIDIA A100/H100 GPU (Ampere/Hopper architecture)
- Complex memory layout transformations
- Limited precision (INT8)

**References**:
- "Darwin-WGA: A Co-processor Provides Increased Sensitivity in Whole Genome Alignments" (2019)
- "GenASM: A High-Performance Genomic Sequence Alignment System" (2020)

**Expected speedup**: 3-5x on Ampere+ GPUs

**Implementation timeline**: 3-6 months (research project)

---

### 7. Edlib Algorithm (2017, still relevant)

**What**: Myers' bit-parallel algorithm implementation

**Why consider**:
- Very fast for small edit distances (1-2% differences)
- Complementary to banded SW
- Already has CPU implementation

**Use case**:
- Exact match extension from seeds
- Final alignment refinement

**Implementation**:
```cpp
// Use edlib for reads with high-quality seeds
if (seed.match_length >= 30 && seed.num_mismatches == 0) {
    // Fast path: edlib bit-parallel alignment
    result = edlib_align_gpu(read, ref_window);
} else {
    // Slower path: banded Smith-Waterman
    result = smith_waterman_banded_warp(read, ref_window);
}
```

**Expected speedup**: 2-3x for high-quality alignments

---

### 8. GASAL2 - GPU-Accelerated Sequence Alignment Library

**What**: Modern GPU alignment library (2019)
**Features**:
- Optimized banded SW
- Support for different scoring schemes
- Batch processing optimizations

**Why consider**:
- Production-ready GPU code
- Active development
- Could replace current SW implementation

**Integration approach**:
- Replace smith_waterman_banded_warp_kernel with GASAL2
- Keep existing pipeline structure

**Expected speedup**: 1.5-2x over current implementation

---

## 📈 Recommended Implementation Roadmap

### Phase 5A: Quick Wins (1-2 weeks)

**Priority 1** ⭐:
1. Implement seed chaining (3-5x speedup)
2. Add seed quality filtering (1.5-2x speedup)
**Expected combined speedup**: 4-8x

**Priority 2**:
3. Implement adaptive band width (2x speedup)
4. Optimize memory access patterns
**Expected combined speedup**: 2-3x additional

**Total Phase 5A speedup**: 8-24x potential

---

### Phase 5B: Algorithm Improvements (1-2 months)

**Priority 3**:
5. Integrate WFA for high-quality alignments
6. Add edlib for exact match extension
**Expected speedup**: 2-3x additional

---

### Phase 6: Advanced (3-6 months, research)

**Priority 4** (optional):
7. Tensor Core acceleration (requires Ampere+ GPU)
8. GASAL2 integration
**Expected speedup**: 2-5x additional

---

## 🛠️ Practical Implementation: Step-by-Step

### Step 1: Add Seed Chaining (Highest Impact)

**File**: `src/cuda/seeding_chain.cu` (new file)

```cpp
#include "winalign/cuda/seeding.cuh"
#include <cuda_runtime.h>

namespace winalign {
namespace cuda {

// Kernel: Chain co-linear seeds (minimap2 algorithm)
__global__ void chain_seeds_kernel(
    const Seed* seeds,
    const uint32_t* seeds_per_read_offsets,
    const uint32_t* seeds_per_read_counts,
    uint32_t num_reads,
    Seed* best_seeds_out,    // One best seed per read
    float* chain_scores_out
) {
    uint32_t read_id = blockIdx.x * blockDim.x + threadIdx.x;
    if (read_id >= num_reads) return;

    uint32_t start = seeds_per_read_offsets[read_id];
    uint32_t count = seeds_per_read_counts[read_id];

    if (count == 0) {
        chain_scores_out[read_id] = 0;
        return;
    }

    if (count == 1) {
        best_seeds_out[read_id] = seeds[start];
        chain_scores_out[read_id] = seeds[start].match_length;
        return;
    }

    // Dynamic programming to find best chain
    float best_score = 0;
    uint32_t best_idx = start;

    for (uint32_t i = 0; i < count; i++) {
        const Seed& curr_seed = seeds[start + i];
        float score = curr_seed.match_length;  // Base score

        // Check for chaining with previous seeds
        for (uint32_t j = 0; j < i; j++) {
            const Seed& prev_seed = seeds[start + j];

            // Check if seeds are co-linear
            int64_t ref_gap = (int64_t)curr_seed.position - (int64_t)prev_seed.position;
            int64_t read_gap = (int64_t)curr_seed.read_offset - (int64_t)prev_seed.read_offset;

            if (ref_gap > 0 && read_gap > 0) {
                int64_t gap_diff = abs(ref_gap - read_gap);

                // If approximately co-linear (gap difference < threshold)
                if (gap_diff < 100) {
                    score += prev_seed.match_length * 0.4f;  // Chaining bonus
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

cudaError_t chain_seeds(
    const Seed* seeds,
    const uint32_t* seeds_per_read_offsets,
    const uint32_t* seeds_per_read_counts,
    uint32_t num_reads,
    Seed* best_seeds_out,
    float* chain_scores_out,
    cudaStream_t stream
) {
    dim3 block_size(256);
    dim3 grid_size((num_reads + block_size.x - 1) / block_size.x);

    chain_seeds_kernel<<<grid_size, block_size, 0, stream>>>(
        seeds,
        seeds_per_read_offsets,
        seeds_per_read_counts,
        num_reads,
        best_seeds_out,
        chain_scores_out
    );

    return cudaGetLastError();
}

} // namespace cuda
} // namespace winalign
```

**Add to header** `include/winalign/cuda/seeding.cuh`:
```cpp
/**
 * @brief Chain co-linear seeds to find best alignment candidate
 *
 * Implements minimap2-style chaining to reduce number of alignments
 * by identifying the most promising seed chain per read.
 *
 * @param seeds All seeds (sorted by read_id)
 * @param seeds_per_read_offsets Starting index for each read's seeds
 * @param seeds_per_read_counts Number of seeds per read
 * @param num_reads Total number of reads
 * @param best_seeds_out Best seed per read (output)
 * @param chain_scores_out Chaining scores (output)
 * @param stream CUDA stream
 * @return cudaError_t
 */
cudaError_t chain_seeds(
    const Seed* seeds,
    const uint32_t* seeds_per_read_offsets,
    const uint32_t* seeds_per_read_counts,
    uint32_t num_reads,
    Seed* best_seeds_out,
    float* chain_scores_out,
    cudaStream_t stream = 0
);
```

**Integrate into pipeline** (`src/core/pipeline_multistream.cpp`):
```cpp
// After seeding, before alignment
cudaError_t err = cuda::chain_seeds(
    ctx.d_seeds,
    ctx.d_seeds_offsets,
    ctx.d_seeds_counts,
    ctx.read_count,
    ctx.d_best_seeds,  // Allocate this
    ctx.d_chain_scores,  // Allocate this
    ctx.stream
);

// Then align only best seeds instead of all seeds
err = cuda::smith_waterman_align_banded_warp(
    ctx.d_read_batch,
    ctx.d_best_seeds,  // Use best seeds instead of all seeds
    ctx.read_count,    // Align one per read instead of num_seeds
    d_reference_,
    reference_length_,
    sw_params,
    ctx.d_results,
    64,  // band_width
    ctx.stream
);
```

---

### Step 2: Add Adaptive Banding

**Modify** `src/cuda/alignment.cu`:
```cpp
// Add before kernel launch
__global__ void calculate_band_widths_kernel(
    const Seed* seeds,
    uint32_t num_seeds,
    uint32_t* band_widths_out
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= num_seeds) return;

    const Seed& seed = seeds[idx];

    // Adaptive band width based on seed quality
    if (seed.match_length >= 30 && seed.num_mismatches == 0) {
        band_widths_out[idx] = 16;  // Narrow band for perfect seeds
    } else if (seed.match_length >= 20) {
        band_widths_out[idx] = 32;  // Medium band
    } else {
        band_widths_out[idx] = 64;  // Wide band
    }
}
```

---

## 📊 Expected Performance Improvements

| Optimization | Speedup | Difficulty | Priority |
|--------------|---------|------------|----------|
| Seed chaining | 3-5x | Medium | ⭐ HIGH |
| Seed filtering | 1.5-2x | Easy | ⭐ HIGH |
| Adaptive banding | 2x | Easy | HIGH |
| WFA integration | 2-3x | Hard | MEDIUM |
| Edlib integration | 2-3x | Medium | MEDIUM |
| Tensor cores | 3-5x | Very Hard | LOW |

**Total realistic speedup**: 8-20x with Phase 5A + 5B

---

## 🧪 Benchmarking and Validation

### Before optimizing:
```bash
# Baseline performance
time ./winalign align --input reads.fastq --output baseline.bam

# Profile with nvprof
nvprof --print-gpu-trace ./winalign align --input reads.fastq

# Identify bottlenecks:
# - % time in seeding
# - % time in alignment
# - % time in filtering
```

### After each optimization:
```bash
# Compare performance
time ./winalign align --input reads.fastq --output optimized.bam

# Validate correctness
diff <(samtools view baseline.bam) <(samtools view optimized.bam)

# Check speedup
echo "Speedup: baseline_time / optimized_time"
```

---

## 📚 References and Resources

### Key Papers:
1. **Minimap2**: Li, H. (2018). "Minimap2: pairwise alignment for nucleotide sequences"
2. **WFA**: Marco-Sola, S. et al. (2020). "Fast gap-affine pairwise alignment using the wavefront algorithm"
3. **Edlib**: Šošić, M. & Šikić, M. (2017). "Edlib: a C/C++ library for fast, exact sequence alignment"
4. **GASAL2**: Ahmed, N. et al. (2019). "GASAL2: a GPU accelerated sequence alignment library for high-throughput NGS data"

### Open Source Implementations:
- Minimap2: https://github.com/lh3/minimap2
- WFA2: https://github.com/smarco/WFA2-lib
- Edlib: https://github.com/Martinsos/edlib
- GASAL2: https://github.com/nahmedraja/GASAL2

---

## ✅ Action Items

**Immediate** (this week):
- [ ] Profile current pipeline to confirm bottlenecks
- [ ] Implement seed chaining kernel
- [ ] Add seed quality filtering

**Short-term** (next 2 weeks):
- [ ] Implement adaptive band width
- [ ] Benchmark and validate improvements
- [ ] Document performance gains

**Medium-term** (next 1-2 months):
- [ ] Research WFA GPU implementation
- [ ] Consider GASAL2 integration
- [ ] Explore edlib for exact matches

**Long-term** (optional):
- [ ] Tensor Core research (if hardware available)
- [ ] Continuous benchmarking vs. BWA-MEM2, minimap2

---

**Status**: 📋 Ready to implement
**Estimated Total Speedup**: 8-20x
**Highest Impact**: Seed chaining (3-5x) + filtering (1.5-2x)
**Recommended First Step**: Implement seed chaining this week
