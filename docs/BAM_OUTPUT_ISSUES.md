# Pipeline BAM Output Issues - Root Cause Analysis

**Date**: 2025-11-19
**Status**: 🔴 CRITICAL BUGS IDENTIFIED
**Severity**: HIGH - Explains inconsistent BAM output

---

## 🚨 Critical Issues Found

### Issue 1: **WRONG BAM WRITER API** (CRITICAL)
**Location**: `src/core/pipeline_multistream.cpp:527`

**Problem**:
```cpp
// Line 527 - WRONG API!
bam_writer_->write_alignment(aln);
```

**The method `write_alignment()` DOES NOT EXIST!**

**Correct API** (from bam_writer.h):
```cpp
Result<bool> write(const Alignment& alignment, const Read& read);
```

**Impact**:
- Code won't compile with this call
- If it's compiling, it means this code path isn't actually being executed
- This explains why BAM files aren't being consistently generated

**Fix Required**:
```cpp
// Need to call write() with both alignment AND read
const Read& read = view.read ? *view.read : (/* get read from context */);
Result<bool> result = bam_writer_->write(aln, read);
if (!result.is_ok()) {
    Logger::instance().error("Failed to write alignment: " + result.error_message());
}
```

---

### Issue 2: **NO ERROR HANDLING** (HIGH)

**Location**: `src/core/pipeline_multistream.cpp:526-528`

**Problem**:
```cpp
// Write to BAM
if (bam_writer_) {
    bam_writer_->write_alignment(aln);  // No error check!
}
```

**Impact**:
- If BAM write fails, it silently continues
- No logging of failures
- User has no idea writes are failing
- Partial BAM files with missing alignments

**Fix Required**:
- Check return value from write()
- Log errors
- Consider accumulating errors and reporting

---

### Issue 3: **POTENTIAL RACE CONDITION** (MEDIUM-HIGH)

**Location**: Multi-stream scheduler loop, lines 204-223

**Problem**:
Multiple GPU contexts can be in `DONE` state simultaneously and all try to write to the same `bam_writer_` concurrently.

```cpp
// Line 204-223 - Multiple contexts processed in same iteration
for (auto& ctx : gpu_contexts_) {
    if (ctx.state == ContextState::DONE) {
        // Process results (aggregate, write BAM)
        process_context_results(ctx);  // ← Multiple contexts calling this!
        ...
    }
}
```

**Evidence from BamWriter.h**:
- No mutex/locks visible
- No thread-safety documentation
- htslib (underlying library) is NOT thread-safe for writes to same file

**Impact**:
- Concurrent writes can corrupt BAM file
- Can cause crashes or hangs
- Explains "needs to be restarted often"
- Explains inconsistent output

**Fix Required**:
- Add mutex around BAM write operations
- OR serialize processing (only one context at a time)
- OR use thread-safe buffering

---

### Issue 4: **MISSING READ DATA** (MEDIUM)

**Location**: `src/core/pipeline_multistream.cpp:505, 521`

**Problem**:
The alignment building code doesn't have access to the actual `Read` object needed for `bam_writer_->write(aln, read)`.

```cpp
// Line 505 - Build alignment from GPU result
aln = batch_helpers_.build_alignment_from_gpu_result(best_result, view);

// Line 527 - Try to write
bam_writer_->write_alignment(aln);  // MISSING: where's the Read?
```

The `view` has a pointer to the read (`view.read`), but we need to dereference it and handle the paired-end case properly.

**Fix Required**:
```cpp
// Get the actual read for BAM writing
const Read& read = [&]() -> const Read& {
    if (ctx.is_paired) {
        size_t pair_idx = i / 2;
        bool is_second = (i % 2 == 1);
        return is_second ? ctx.host_pairs[pair_idx].read2
                        : ctx.host_pairs[pair_idx].read1;
    } else {
        return ctx.host_singles[i];
    }
}();

Result<bool> result = bam_writer_->write(aln, read);
```

---

### Issue 5: **SYNCHRONIZATION ASSUMPTIONS** (LOW-MEDIUM)

**Location**: `src/core/pipeline_multistream.cpp:197-202`

**Problem**:
The code assumes that after `check_event(event_d2h_done)` succeeds, the pinned memory is immediately safe to access. While this should be true, there's no explicit synchronization.

```cpp
// Line 197-202
if (check_event(ctx.event_d2h_done, ctx, all_contexts_done)) {
    record_timing(ctx.event_sw_done, ctx.event_d2h_done, ctx.timing.t_d2h);
    ctx.state = ContextState::DONE;
    all_contexts_done = false;
}

// Line 206 - Immediately processes in same iteration
process_context_results(ctx);
```

**Potential Issue**:
- `cudaEventQuery` returns success when event is recorded, but doesn't guarantee memory visibility
- Should use `cudaEventSynchronize` or `cudaStreamSynchronize` for safety

**Fix**:
Add explicit sync before accessing pinned memory in `process_context_results`.

---

## 📊 Impact Summary

| Issue | Severity | Impact on BAM Output |
|-------|----------|---------------------|
| Wrong API call | CRITICAL | Prevents compilation or execution |
| No error handling | HIGH | Silent failures, incomplete files |
| Race condition | HIGH | Corrupted files, crashes, hangs |
| Missing read data | MEDIUM | Can't call correct API |
| Sync assumptions | LOW | Possible memory corruption (rare) |

---

## 🔧 Recommended Fixes (Priority Order)

### Fix 1: Correct BAM Writer API Call (CRITICAL)

**File**: `src/core/pipeline_multistream.cpp`

**Replace lines 508-528:**
```cpp
Alignment aln;
const Read* current_read_ptr = nullptr;

if (found && best_result.score > 0) {
    // Build alignment from GPU result
    aln = batch_helpers_.build_alignment_from_gpu_result(best_result, view);
    batch_helpers_.update_metrics(true, aln.mapq);
    current_read_ptr = view.read;
} else {
    // Build unmapped alignment
    bool is_second = false;

    if (ctx.is_paired) {
        size_t pair_idx = i / 2;
        is_second = (i % 2 == 1);
        current_read_ptr = is_second ? &ctx.host_pairs[pair_idx].read2
                                      : &ctx.host_pairs[pair_idx].read1;
    } else {
        current_read_ptr = &ctx.host_singles[i];
    }

    aln = batch_helpers_.build_unmapped_alignment(*current_read_ptr, ctx.is_paired, is_second);
    batch_helpers_.update_metrics(false, 0);
}

// Write to BAM with proper error handling
if (bam_writer_ && current_read_ptr) {
    Result<bool> write_result = bam_writer_->write(aln, *current_read_ptr);
    if (!write_result.is_ok()) {
        Logger::instance().error("Failed to write alignment for read " +
                                current_read_ptr->name + ": " +
                                write_result.error_message());
    }
}
```

### Fix 2: Add Thread Safety for BAM Writing (HIGH)

**File**: `src/core/pipeline_multistream.h`

Add mutex:
```cpp
private:
    // ... existing members ...
    std::mutex bam_write_mutex_;  // ADD THIS
```

**File**: `src/core/pipeline_multistream.cpp`

Protect writes:
```cpp
// Write to BAM with thread safety
if (bam_writer_ && current_read_ptr) {
    std::lock_guard<std::mutex> lock(bam_write_mutex_);
    Result<bool> write_result = bam_writer_->write(aln, *current_read_ptr);
    if (!write_result.is_ok()) {
        Logger::instance().error("Failed to write alignment for read " +
                                current_read_ptr->name + ": " +
                                write_result.error_message());
    }
}
```

### Fix 3: Add Explicit Synchronization (MEDIUM)

**File**: `src/core/pipeline_multistream.cpp`

**Add at start of `process_context_results`:**
```cpp
void MultiStreamScheduler::process_context_results(GpuBatchContext& ctx) {
    using namespace std::chrono;
    auto write_start = high_resolution_clock::now();

    // Ensure D2H transfer is fully complete
    if (ctx.stream) {
        cudaStreamSynchronize(ctx.stream);
    }

    // Copy GPU results to host
    ctx.host_results.resize(ctx.num_seeds);
    // ... rest of function
}
```

---

## 🎯 Expected Results After Fixes

✅ **Consistent BAM output** - Every run produces complete file
✅ **No crashes/hangs** - Thread-safe writes prevent corruption
✅ **Error visibility** - Failed writes are logged
✅ **Data integrity** - Proper sync ensures correct data
✅ **No restarts needed** - Stable execution

---

## 🧪 Testing Plan

1. **Compile test**: Verify code compiles with correct API
2. **Single-threaded test**: Run with 1 GPU context
3. **Multi-threaded test**: Run with 3 GPU contexts (stress test)
4. **Error injection**: Test with disk full, permission errors
5. **Validate output**: Compare BAM records count vs input reads

---

**Status**: 🔴 **CRITICAL BUGS IDENTIFIED** - Fixes required
**Root Cause**: Wrong API call + race condition + no error handling
**Estimated Fix Time**: 1-2 hours
**Priority**: IMMEDIATE - Blocks production use
