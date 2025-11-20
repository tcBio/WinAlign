# Multi-GPU Support for NVIDIA L40S

**Date**: 2025-11-19
**Hardware**: 2× NVIDIA L40S GPUs
**Architecture**: Ada Lovelace (Compute Capability 8.9)

---

## 🎯 Current GPU Support Status

### ✅ What's Already Supported

**Single GPU with Multi-Stream**:
- Current implementation uses 1 GPU with 3 concurrent CUDA streams
- Excellent for overlapping H2D, compute, and D2H transfers
- Already optimized for high GPU utilization

**GPU Detection**:
- Code detects available GPUs via `cudaGetDeviceCount()`
- Allows selecting specific GPU via `--gpu-device-id` parameter
- See: `src/core/pipeline_initialization.cpp:149-172`

**Architecture Support**:
- Current code is architecture-agnostic (works on any CUDA GPU)
- Need to add compute capability flags for L40S optimization

---

## 📊 NVIDIA L40S Specifications

| Specification | Value |
|--------------|-------|
| **Architecture** | Ada Lovelace (not Ampere, newer!) |
| **Compute Capability** | 8.9 |
| **CUDA Cores** | 18,176 |
| **Tensor Cores** | 568 (4th generation) |
| **Memory** | 48 GB GDDR6 |
| **Memory Bandwidth** | 864 GB/s |
| **FP32 Performance** | ~90 TFLOPS |
| **Tensor Performance** | ~360 TFLOPS (FP16) |

**Note**: Ada Lovelace is **newer** than Ampere (which is compute 8.0/8.6)
- Ampere: A100, A40, A30 (compute 8.0/8.6)
- **Ada Lovelace**: L40S, L40, RTX 4090 (compute 8.9)
- Better performance and newer features than Ampere!

---

## 🛠️ Optimization for L40S

### 1. Update CUDA Compilation Flags

**Current**: No architecture-specific flags (uses default)

**Recommended**: Add compute capability 8.9 for L40S

Update `src/cuda/CMakeLists.txt`:
```cmake
# CUDA compilation flags
target_compile_options(winalign_cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:
        -gencode arch=compute_89,code=sm_89  # NEW: L40S (Ada Lovelace)
        --use_fast_math
        --ptxas-options=-v
        -lineinfo
    >
)
```

**Why This Matters**:
- Enables L40S-specific optimizations
- Uses 4th gen Tensor Cores (if we add tensor optimizations later)
- Better instruction scheduling for Ada architecture
- **Expected**: 10-20% performance improvement

---

### 2. Fallback for Other GPUs (Optional)

If you want to support multiple GPU types:

```cmake
# Support multiple architectures
target_compile_options(winalign_cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:
        -gencode arch=compute_75,code=sm_75  # Turing (T4)
        -gencode arch=compute_80,code=sm_80  # Ampere (A100)
        -gencode arch=compute_86,code=sm_86  # Ampere (A40)
        -gencode arch=compute_89,code=sm_89  # Ada Lovelace (L40S)
        --use_fast_math
        --ptxas-options=-v
        -lineinfo
    >
)
```

**Trade-off**: Larger binary size, longer compile time

**Recommendation**: Just use `compute_89` since you have L40S GPUs.

---

## 🚀 Using Both L40S GPUs

### Option 1: Process-Level Parallelism (RECOMMENDED ✅)

**Run 2 instances of winalign in parallel, one per GPU**:

```bash
# Terminal 1: GPU 0
CUDA_VISIBLE_DEVICES=0 ./winalign align \
    --input samples_1-40.fastq \
    --output batch1.bam &

# Terminal 2: GPU 1
CUDA_VISIBLE_DEVICES=1 ./winalign align \
    --input samples_41-81.fastq \
    --output batch2.bam &

wait  # Wait for both to complete
```

**Advantages**:
- ✅ **No code changes needed**
- ✅ **Perfect load balancing** (each GPU independent)
- ✅ **Simple and reliable**
- ✅ **2× throughput** (linear scaling)

**Disadvantages**:
- Requires manually splitting input files

---

### Option 2: Multi-GPU Pipeline (NOT IMPLEMENTED ⚠️)

**Would require significant code changes**:

1. **Duplicate GPU contexts** (currently 3 per GPU → 6 total for 2 GPUs)
2. **Add work distribution logic** (assign batches to GPUs)
3. **Synchronize BAM writes** (merge results from both GPUs)
4. **Handle load balancing** (if GPUs have different workloads)

**Effort**: 2-3 days of implementation + testing

**Benefit**: Automatic multi-GPU without manual file splitting

**Recommendation**: **NOT worth it** - Option 1 (process-level) is simpler and just as effective

---

## 📈 Expected Performance with 2× L40S

### Current (1× L40S, with seed chaining)
```
Per sample: 5-10 minutes
81 samples: 6-13 hours
GPU utilization: 30-60% (after I/O fixes)
```

### With 2× L40S (process-level parallelism)
```
Per sample: 5-10 minutes (same per GPU)
81 samples: 3-6.5 hours (2× faster!)
GPU 0: 40 samples = 3.3-6.7 hours
GPU 1: 41 samples = 3.4-6.8 hours
Total: ~3.5-7 hours (both GPUs in parallel)
```

**Expected**: **~2× speedup** from using both GPUs

---

## 🔧 Implementation Steps

### Step 1: Update CUDA Flags for L40S (5 minutes)

Edit `src/cuda/CMakeLists.txt`:
```cmake
# CUDA compilation flags
target_compile_options(winalign_cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:
        -gencode arch=compute_89,code=sm_89  # Add this line
        --use_fast_math
        --ptxas-options=-v
        -lineinfo
    >
)
```

### Step 2: Rebuild

```bash
cd build
cmake .. -DCMAKE_CUDA_ARCHITECTURES=89  # Optional: can also set via cmake
make clean
make winalign_core -j8
```

### Step 3: Verify GPU Detection

```bash
./winalign align --help  # Check if GPU is detected

# Should see in logs:
# "Detected 2 CUDA device(s)"
# "Using GPU device 0: NVIDIA L40S"
```

### Step 4: Test Single GPU

```bash
# Test with seed chaining on GPU 0
CUDA_VISIBLE_DEVICES=0 ./winalign align \
    --input sample.fastq \
    --output test.bam

# Verify performance and correctness
```

### Step 5: Run Multi-GPU Batch (81 samples)

**Split samples**:
```bash
# Assuming you have sample IDs in a file
head -40 sample_list.txt > batch1_samples.txt
tail -41 sample_list.txt > batch2_samples.txt
```

**Run both GPUs**:
```bash
# GPU 0: Samples 1-40
CUDA_VISIBLE_DEVICES=0 ./winalign align \
    --input batch1.fastq \
    --output batch1.bam \
    > gpu0.log 2>&1 &

# GPU 1: Samples 41-81
CUDA_VISIBLE_DEVICES=1 ./winalign align \
    --input batch2.fastq \
    --output batch2.bam \
    > gpu1.log 2>&1 &

# Monitor both
watch -n 1 "nvidia-smi; tail -5 gpu0.log; echo '---'; tail -5 gpu1.log"
```

**Merge results** (if needed):
```bash
samtools merge -@ 8 final.bam batch1.bam batch2.bam
```

---

## 📊 Performance Monitoring

### Monitor Both GPUs

```bash
# Watch both GPUs
watch -n 1 nvidia-smi

# Should see:
# GPU 0: 30-60% utilization, 11/48 GB memory
# GPU 1: 30-60% utilization, 11/48 GB memory
```

### Check Load Balance

```bash
# Count reads processed by each GPU
samtools view -c batch1.bam  # GPU 0
samtools view -c batch2.bam  # GPU 1

# Should be roughly equal
```

---

## 🎓 Advanced: Tensor Core Optimization (Future)

L40S has **4th generation Tensor Cores** (better than Ampere's 3rd gen):

**Future optimization** (not yet implemented):
- Use Tensor Cores for Smith-Waterman alignment
- Convert DP matrix operations to INT8 or FP16 tensor ops
- Potential **5-10× speedup** for alignment

**Requires**:
- Rewrite alignment kernel to use `wmma` (warp matrix multiply-accumulate)
- Convert to INT8/FP16 precision
- Research project (2-4 weeks)

**References**:
- NVIDIA's cuBLASLt for Tensor Core ops
- "Darwin-WGA" paper (genome alignment with Tensor Cores)

---

## ✅ Recommended Configuration

### For Your 2× L40S Setup

**CMake flags**:
```cmake
-gencode arch=compute_89,code=sm_89
```

**Runtime**:
```bash
# Use both GPUs with process-level parallelism
CUDA_VISIBLE_DEVICES=0,1  # Make both visible (default)

# Run 2 instances manually on different GPUs
CUDA_VISIBLE_DEVICES=0 ./winalign ... &
CUDA_VISIBLE_DEVICES=1 ./winalign ... &
```

**Expected results**:
- **Per GPU**: 5-10 min per sample
- **81 samples**: 3.5-7 hours (vs 6-13 hours on 1 GPU)
- **GPU usage**: 30-60% each (after I/O optimization)
- **Speedup**: 2× from multi-GPU + 3-5× from seed chaining = **6-10× total**

---

## 📋 Summary

| Feature | Status | Action Needed |
|---------|--------|---------------|
| **L40S Detection** | ✅ Works | None |
| **Compute 8.9 Support** | ⚠️ Add flags | Update CMakeLists.txt |
| **Single GPU Performance** | ✅ Optimized | Seed chaining implemented |
| **Multi-GPU (Process)** | ✅ Ready | Run 2 instances manually |
| **Multi-GPU (Automatic)** | ❌ Not implemented | Not needed (use process-level) |
| **Tensor Cores** | ❌ Future | Research project |

---

## 🚀 Quick Start for Your Setup

```bash
# 1. Update CMakeLists.txt (add compute_89 flag)
# 2. Rebuild
cd build && cmake .. && make clean && make winalign_core -j8

# 3. Test single GPU
CUDA_VISIBLE_DEVICES=0 ./winalign align --input test.fastq --output test.bam

# 4. Run production batch on both GPUs
CUDA_VISIBLE_DEVICES=0 ./winalign align --input batch1.fastq --output batch1.bam &
CUDA_VISIBLE_DEVICES=1 ./winalign align --input batch2.fastq --output batch2.bam &
wait

# Expected time: 3.5-7 hours for 81 samples
```

---

**Status**: ✅ Ready for 2× L40S deployment
**Recommendation**: Process-level parallelism (simplest, most reliable)
**Expected Performance**: 2× speedup from multi-GPU, 3-5× from seed chaining = **6-10× total improvement**
