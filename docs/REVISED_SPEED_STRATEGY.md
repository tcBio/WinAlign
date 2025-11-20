# WinAlign Speed Optimization - REVISED Strategy Based on Real Performance Data

**Date**: 2025-11-19
**Status**: ⚠️ CRITICAL REVISION - Based on actual performance metrics
**Problem**: 110 minutes per sample with **4% GPU utilization**

---

## 🚨 CRITICAL FINDING: Wrong Bottleneck Identified

### Original Assumption (INCORRECT)
- Assumed: Smith-Waterman alignment is bottleneck (60-70% of time)
- Strategy: Seed chaining, adaptive banding
- Expected: High GPU utilization

### **Actual Performance Data**
```
Sample processing time: 110 minutes per sample
GPU utilization: 4% ⚠️ EXTREMELY LOW
GPU memory: 11.4 GB allocated
Speed: 0.9% progress per minute
81 samples: 148 hours (6.2 days)
```

### **Root Cause Analysis**

**4% GPU utilization means**:
- ❌ Alignment is NOT the bottleneck
- ❌ GPU is starving (not enough work)
- ✅ Pipeline is I/O bound or CPU bound
- ✅ GPU sits idle most of the time

**Possible bottlenecks** (in order of likelihood):

1. **I/O Bottleneck** (MOST LIKELY)
   - Reading FASTQ files (disk I/O)
   - Writing BAM files (disk I/O)
   - Small batch sizes → GPU starvation

2. **CPU-side Seeding** (LIKELY)
   - FM-index operations on CPU
   - Not enough GPU work generated

3. **Memory Transfer Overhead** (POSSIBLE)
   - H2D/D2H transfers taking too long
   - Not overlapped with compute

4. **Small Batch Sizes** (POSSIBLE)
   - GPU processes batches too quickly
   - Spends most time waiting for data

5. **Synchronization Issues** (LESS LIKELY)
   - Excessive cudaStreamSynchronize calls
   - Prevents overlap

---

## 🔍 Diagnostic Steps (URGENT)

### Step 1: Check Current Configuration

**Check batch size**:
```bash
# What is the current batch size?
grep -r "batch_size" /home/user/WinAlign/src/main.cpp
grep -r "default.*batch" /home/user/WinAlign/include/

# Current setting likely: 1,000-10,000 reads/batch
# Optimal for GPU: 50,000-100,000 reads/batch
```

**Check GPU contexts**:
```cpp
// From pipeline_internal.h
constexpr int NUM_GPU_CONTEXTS = 3;  // Good - overlaps 3 batches
```

### Step 2: Profile the Pipeline

**Add timing instrumentation**:
```cpp
// In pipeline_multistream.cpp, add timing for each stage:

auto stage_start = std::chrono::high_resolution_clock::now();

// === LOAD STAGE ===
auto load_start = now();
// ... load FASTQ batch ...
auto load_end = now();
double load_time = duration(load_start, load_end);

// === H2D TRANSFER ===
auto h2d_start = now();
// ... cudaMemcpyAsync ...
auto h2d_end = now();
double h2d_time = duration(h2d_start, h2d_end);

// === SEEDING ===
auto seed_start = now();
// ... generate_gpu_seeds ...
auto seed_end = now();
double seed_time = duration(seed_start, seed_end);

// === ALIGNMENT ===
auto align_start = now();
// ... smith_waterman_align ...
auto align_end = now();
double align_time = duration(align_start, align_end);

// === D2H TRANSFER ===
auto d2h_start = now();
// ... cudaMemcpyAsync ...
auto d2h_end = now();
double d2h_time = duration(d2h_start, d2h_end);

// === BAM WRITE ===
auto bam_start = now();
// ... bam_writer->write() ...
auto bam_end = now();
double bam_time = duration(bam_start, bam_end);

// Log breakdown
Logger::instance().info(
    "Timing breakdown: " +
    "Load=" + std::to_string(load_time) + "ms, " +
    "H2D=" + std::to_string(h2d_time) + "ms, " +
    "Seed=" + std::to_string(seed_time) + "ms, " +
    "Align=" + std::to_string(align_time) + "ms, " +
    "D2H=" + std::to_string(d2h_time) + "ms, " +
    "BAM=" + std::to_string(bam_time) + "ms"
);
```

**Run profiling**:
```bash
# Option 1: Use nvidia-smi to monitor GPU
watch -n 1 nvidia-smi

# Option 2: Use nvprof (if available)
nvprof --print-gpu-trace ./winalign align --input sample.fastq

# Option 3: Add custom timing (recommended)
# Rebuild with timing instrumentation above
```

---

## 🎯 Likely Solutions (Based on 4% GPU Usage)

### Solution 1: Increase Batch Size (HIGHEST PRIORITY ⭐⭐⭐)

**Problem**: Small batches → GPU finishes quickly → sits idle waiting for next batch

**Current**: Likely 1,000-10,000 reads/batch
**Target**: 50,000-100,000 reads/batch

**Implementation**:
```cpp
// In main.cpp or config
PipelineConfig config;
config.batch_size = 50000;  // INCREASE from default (likely 10000)

// OR command line:
./winalign align --batch-size 50000 --input sample.fastq
```

**Expected Impact**:
- Keeps GPU busy longer per batch
- Reduces I/O overhead (fewer batch switches)
- Better amortization of transfer overhead
- **Potential speedup: 3-10x** if I/O bound

**Risk**: Uses more GPU memory (monitor with nvidia-smi)

---

### Solution 2: Optimize FASTQ Reading (HIGH PRIORITY ⭐⭐)

**Problem**: Reading FASTQ from disk is slow, GPU waits for data

**Current**: FastqWorker with 4 I/O threads (good)

**Improvements**:

**A. Use memory-mapped I/O**:
```cpp
// Instead of fread(), use mmap()
#include <sys/mman.h>

int fd = open(fastq_path, O_RDONLY);
struct stat sb;
fstat(fd, &sb);
char* mapped = (char*)mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

// Advise kernel for sequential access
madvise(mapped, sb.st_size, MADV_SEQUENTIAL);

// Parse directly from mapped memory (much faster)
```

**B. Prefetch more batches**:
```cpp
// In pipeline_multistream.cpp:75
NUM_GPU_CONTEXTS + 3  // Prefetch 3 extra batches instead of 1
```

**C. Use faster parser** (if current parser is slow):
- Consider kseq.h (very fast C parser)
- Or parallel parsing with multiple threads

**Expected Impact**: 2-5x faster I/O → 2-3x overall speedup

---

### Solution 3: Optimize BAM Writing (HIGH PRIORITY ⭐⭐)

**Problem**: Writing BAM to disk blocks the pipeline

**Current**: Single-threaded BAM write with mutex (thread-safe but slow)

**Improvements**:

**A. Asynchronous BAM writing**:
```cpp
// Create separate BAM writer thread
std::thread bam_writer_thread([this]() {
    while (running_) {
        AlignmentBatch batch = bam_queue_.pop();  // Blocking queue
        for (auto& aln : batch) {
            bam_writer_->write(aln.alignment, aln.read);
        }
    }
});

// In GPU results processing:
// Instead of writing directly, queue for async write
bam_queue_.push(AlignmentBatch{alignments, reads});
// Continue immediately (don't wait for write)
```

**B. Buffer BAM writes**:
```cpp
// Write in larger chunks instead of per-alignment
std::vector<Alignment> buffer;
buffer.reserve(10000);

for (auto& aln : alignments) {
    buffer.push_back(aln);
    if (buffer.size() >= 10000) {
        bam_writer_->write_batch(buffer);  // Write 10k at once
        buffer.clear();
    }
}
```

**Expected Impact**: 2-3x faster BAM writing → 1.5-2x overall speedup

---

### Solution 4: Increase GPU Context Overlap (MEDIUM PRIORITY ⭐)

**Problem**: Only 3 GPU contexts may not provide enough overlap

**Current**: NUM_GPU_CONTEXTS = 3

**Increase to 6-8**:
```cpp
// In pipeline_internal.h
constexpr int NUM_GPU_CONTEXTS = 6;  // More overlap
```

**Trade-off**: Uses more GPU memory, but better overlap

**Expected Impact**: 1.3-1.5x speedup if memory bottleneck

---

### Solution 5: Remove Unnecessary Synchronization (LOW PRIORITY)

**Problem**: Excessive cudaStreamSynchronize() prevents overlap

**Check current code**:
```bash
grep -n "cudaStreamSynchronize" src/core/pipeline_multistream.cpp
# Line 237: After kernel launch (may be unnecessary)
# Line 480: Before accessing pinned memory (necessary)
```

**Only synchronize when absolutely required**:
- Before reading GPU results (necessary)
- NOT after every kernel launch (unnecessary)

**Expected Impact**: 1.2-1.3x speedup

---

## 📊 Revised Implementation Priority

| Solution | Impact | Effort | Priority | Timeline |
|----------|--------|--------|----------|----------|
| **Increase batch size** | 3-10x | LOW | ⭐⭐⭐ CRITICAL | 5 min |
| **Profile pipeline** | N/A | LOW | ⭐⭐⭐ CRITICAL | 1 day |
| **Optimize FASTQ I/O** | 2-3x | MEDIUM | ⭐⭐ HIGH | 2-3 days |
| **Async BAM writing** | 1.5-2x | MEDIUM | ⭐⭐ HIGH | 2-3 days |
| **Increase GPU contexts** | 1.3-1.5x | LOW | ⭐ MEDIUM | 1 hour |
| **Remove excess sync** | 1.2-1.3x | LOW | ⭐ MEDIUM | 1 hour |

**Seed chaining (original plan)**: DEFERRED until GPU utilization is >50%

---

## 🚀 Immediate Action Plan

### Today (1 hour)

**1. Try larger batch size** (5 minutes):
```bash
# Kill current run (if still running)
# Test with large batch size
./winalign align \
    --batch-size 50000 \
    --input sample.fastq \
    --output test.bam

# Monitor GPU utilization
watch -n 1 nvidia-smi
# Look for GPU usage > 20%
```

**2. Add timing instrumentation** (30 minutes):
- Add timing logs to pipeline_multistream.cpp (code above)
- Rebuild
- Run one sample to see breakdown

**3. Check results** (30 minutes):
- If GPU usage increases → batch size was the issue
- If still 4% → I/O or CPU bottleneck
- Review timing breakdown to identify bottleneck

---

### This Week

**Day 1**: Profile and diagnose
- ✅ Identify exact bottleneck from timing data
- ✅ Test larger batch sizes
- ✅ Measure GPU utilization improvement

**Day 2-3**: Implement highest impact fix
- If I/O bound → Optimize FASTQ reading (mmap)
- If BAM bound → Async BAM writing
- If still unclear → Add more detailed profiling

**Day 4-5**: Test and validate
- Benchmark improvements
- Ensure correctness
- Measure speedup

---

## 📈 Expected Results

### Current Performance
```
110 minutes per sample
4% GPU utilization
81 samples = 148 hours (6.2 days)
```

### After Batch Size Increase
```
30-40 minutes per sample (3x speedup)
15-30% GPU utilization
81 samples = 40-54 hours (1.7-2.3 days)
```

### After I/O Optimization
```
15-20 minutes per sample (5-7x speedup)
30-50% GPU utilization
81 samples = 20-27 hours (0.8-1.1 days)
```

### After Full Optimization
```
5-10 minutes per sample (10-20x speedup)
50-80% GPU utilization
81 samples = 6-13 hours (0.25-0.5 days)
```

---

## ✅ Key Insights

### Why Original Plan Was Wrong

**Original assumption**:
- Alignment is 60-70% of time → optimize alignment

**Reality (from 4% GPU usage)**:
- GPU sits idle 96% of time
- Alignment finishes quickly
- Pipeline waits for I/O

**Lesson**: Profile first, optimize second

### What 4% GPU Usage Tells Us

1. **GPU is not the bottleneck** - it's starving for work
2. **I/O or CPU is the bottleneck** - likely disk or parsing
3. **Batch sizes may be too small** - GPU finishes too quickly
4. **Pipeline overlap insufficient** - not enough prefetch

### Critical Next Step

**DO NOT implement seed chaining yet!**
- It optimizes GPU compute (not the bottleneck)
- Won't help if GPU is idle 96% of time
- First fix the 4% utilization problem

**DO implement**:
1. Larger batch sizes (immediate test)
2. Profile to identify exact bottleneck
3. Optimize I/O or BAM writing based on data
4. THEN consider GPU optimizations when utilization >50%

---

## 🎓 Revised Strategy Summary

### Phase 1 (This Week): Fix Pipeline Starvation
- **Goal**: Increase GPU utilization from 4% → 30-50%
- **Approach**: Larger batches, better I/O, async BAM writes
- **Expected**: 5-10x speedup (110 min → 10-20 min)

### Phase 2 (Next Week): Optimize I/O
- **Goal**: Eliminate I/O bottleneck
- **Approach**: mmap, parallel parsing, buffered BAM writes
- **Expected**: Additional 2-3x speedup

### Phase 3 (Later): GPU Optimizations
- **Goal**: Optimize compute when GPU is actually busy
- **Approach**: Seed chaining, adaptive banding
- **Expected**: Additional 2-3x speedup
- **Only do this when GPU utilization >50%**

---

**Status**: 🔴 CRITICAL - Immediate action required
**Priority**: Test large batch size TODAY (5 minute test)
**Expected Result**: 110 min → 10-20 min per sample (10x improvement)
**Next Step**: Run with --batch-size 50000 and monitor GPU utilization
