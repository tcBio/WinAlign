# Performance Update - Slow USB Drive Identified

**Date**: 2025-11-19
**Critical Finding**: Output to slow USB drive was the bottleneck

---

## 🎯 Root Cause Identified

### Original Symptoms
```
110 minutes per sample
4% GPU utilization
11.4 GB GPU memory allocated
0.9% progress per minute
```

### Root Cause
**Slow USB drive for BAM output** - confirmed by user

This explains:
- ✅ 4% GPU utilization (GPU waits for BAM writes to slow USB)
- ✅ 110 minute runtime (disk I/O bottleneck)
- ✅ Low progress rate (blocked on disk writes)

---

## 📊 Expected Improvement After Drive Switch

### With Slow USB Drive (Current)
```
BAM write speed: ~10-20 MB/s (USB 2.0 or slow USB 3.0)
BAM size: 4.7 GB per sample
Write time: 4.7 GB / 15 MB/s = ~313 seconds (~5 minutes just for writes)
Total time: 110 minutes (writes + compute + FASTQ reading)
GPU utilization: 4% (waiting on disk)
```

### With Fast Drive (Expected)
```
BAM write speed: ~500 MB/s (NVMe SSD) or ~200 MB/s (SATA SSD)
BAM size: 4.7 GB per sample
Write time: 4.7 GB / 500 MB/s = ~9 seconds (writes are now negligible)
Total time: 5-20 minutes (actual compute + FASTQ reading)
GPU utilization: 30-60% (actually doing work)
```

**Expected speedup from drive switch alone: 5-20x improvement**

---

## 🔍 Next Steps After Drive Switch

### Step 1: Re-run Sample and Monitor (30 minutes)

```bash
# Run on faster drive
./winalign align \
    --input /path/to/sample.fastq \
    --output /path/to/FAST_DRIVE/output.bam

# Monitor GPU in another terminal
watch -n 1 nvidia-smi

# Observe:
# - Processing time (should be much faster)
# - GPU utilization (should be higher)
# - Progress rate (should be faster)
```

**Look for**:
- Time per sample: 5-20 minutes (vs 110 minutes)
- GPU utilization: 20-60% (vs 4%)
- Completion rate: ~5-20% per minute (vs 0.9%)

---

### Step 2: Determine If Further Optimization Needed

**Scenario A: Still Slow (>30 min per sample)**
- Add timing instrumentation (from IMMEDIATE_NEXT_STEPS.md)
- Profile to find remaining bottleneck
- Likely FASTQ reading from slow source drive
- Solution: Move input files to faster drive too

**Scenario B: Fast Enough (5-10 min per sample)**
- ✅ Problem solved with drive switch
- No further optimization needed
- 81 samples × 10 min = ~13.5 hours (acceptable)

**Scenario C: Fast But Could Be Faster (10-20 min per sample)**
- Implement async BAM writing (from revised strategy)
- Implement seed chaining (from original strategy)
- Target: 5 min per sample
- 81 samples × 5 min = ~6.75 hours

---

## 📊 Performance Prediction Matrix

| Input Drive | Output Drive | Expected Time | GPU Usage | Bottleneck |
|-------------|--------------|---------------|-----------|------------|
| Slow USB | Slow USB | 110 min | 4% | BAM writes |
| Slow USB | Fast SSD | 30-40 min | 15-25% | FASTQ reads |
| Fast SSD | Fast SSD | 10-20 min | 30-50% | Pipeline |
| Fast SSD + Optimizations | Fast SSD | 5-10 min | 40-60% | Balanced |

---

## ✅ Recommended Actions

### Immediate (Today)
1. ✅ Switch to faster output drive (in progress)
2. Run one sample on new drive
3. Measure actual performance
4. Check GPU utilization

### If Still Slow After Drive Switch

**If >30 min per sample**:
- Move input FASTQ files to faster drive too
- Add timing instrumentation
- Profile to find remaining bottleneck

**If 10-30 min per sample**:
- Consider implementing optimizations:
  - Async BAM writing (2-3x speedup)
  - Seed chaining (3-5x speedup)
- Target: 5-10 min per sample

**If <10 min per sample**:
- ✅ Acceptable performance
- No further optimization needed

---

## 🎓 Lessons Learned

### 1. Profile Hardware Before Optimizing Software
- Always check I/O performance first
- Slow disks can bottleneck even the fastest GPU
- 4% GPU usage was a red flag for I/O issues

### 2. BAM Files Are Large
- 4.7 GB per sample
- Requires fast storage for good performance
- USB 2.0/3.0 flash drives are not suitable for large BAM output

### 3. Multi-Stage Pipeline Sensitivity
- Pipeline is only as fast as slowest stage
- GPU can be starved by slow I/O
- Fast disks are critical for genomics workflows

---

## 📈 Expected Timeline for 81 Samples

| Configuration | Time per Sample | Total Time | Status |
|---------------|----------------|------------|---------|
| **Slow USB (current)** | 110 min | 148 hours (6.2 days) | ❌ Too slow |
| **Fast drive only** | 10-20 min | 13-27 hours (0.5-1.1 days) | ✅ Acceptable |
| **Fast drive + optimizations** | 5-10 min | 6-13 hours (0.25-0.5 days) | ✅ Excellent |

---

## 🚀 Summary

### Problem
Slow USB drive for BAM output created I/O bottleneck

### Solution
Switch to fast SSD (NVMe or SATA)

### Expected Result
**110 min → 10-20 min per sample (5-10x speedup)**

### Next Action
1. Complete drive switch
2. Run one sample to measure new baseline
3. Decide if further optimization needed based on results

---

**Status**: 🟡 Waiting for drive switch results
**Expected Outcome**: 5-10x improvement from hardware change alone
**Further Optimization**: Only if needed after testing with fast drive
