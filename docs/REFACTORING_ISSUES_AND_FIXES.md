# Pipeline Refactoring - Issues and Fixes

**Date**: 2025-11-19
**Status**: 🔴 BLOCKING ISSUE IDENTIFIED
**Priority**: HIGH - Code won't produce correct output

---

## 🚨 Critical Issue: Alignment Building Not Implemented

### Problem Description

The refactored code **does not produce BAM output** because alignment building logic is missing in `MultiStreamScheduler::process_context_results()`.

**Location**: `src/core/pipeline_multistream.cpp:481`

```cpp
void MultiStreamScheduler::process_context_results(GpuBatchContext& ctx) {
    // ... copy results from GPU ...

    // TODO: Implement alignment building (requires BatchProcessingHelpers)
    Logger::instance().warn("Alignment building not yet implemented");

    // Metrics updated but NO alignments built or written to BAM!
}
```

### Impact

- ✅ Code compiles successfully
- ✅ GPU kernels execute correctly
- ✅ Data transfers work (H2D, D2H)
- ✅ Metrics are collected
- ❌ **NO ALIGNMENTS ARE BUILT**
- ❌ **NO BAM OUTPUT IS WRITTEN**
- ❌ **Pipeline produces empty/incomplete results**

### Root Cause

During refactoring, the alignment building logic was extracted into `BatchProcessingHelpers` module, but `MultiStreamScheduler` **does not have access to this module**.

**What's missing:**
1. `MultiStreamScheduler` doesn't have a reference to `BatchProcessingHelpers`
2. The `process_context_results()` method can't call alignment building functions
3. No BAM writing occurs because no alignments are created

---

## 🔧 Required Fixes

### Fix 1: Add BatchProcessingHelpers Dependency

**File**: `src/core/pipeline_multistream.h`

**Current constructor:**
```cpp
MultiStreamScheduler(
    const PipelineConfig& config,
    std::vector<GpuBatchContext>& contexts,
    MetricsCollector& metrics,
    std::atomic<bool>& cancelled);
```

**Required change:**
```cpp
MultiStreamScheduler(
    const PipelineConfig& config,
    std::vector<GpuBatchContext>& contexts,
    MetricsCollector& metrics,
    BatchProcessingHelpers& batch_helpers,  // ADD THIS
    std::atomic<bool>& cancelled);
```

**Add member variable:**
```cpp
private:
    // ... existing members ...
    MetricsCollector& metrics_;
    BatchProcessingHelpers& batch_helpers_;  // ADD THIS
    std::atomic<bool>& cancelled_;
```

### Fix 2: Update Constructor Implementation

**File**: `src/core/pipeline_multistream.cpp`

**Update constructor:**
```cpp
MultiStreamScheduler::MultiStreamScheduler(
    const PipelineConfig& config,
    std::vector<GpuBatchContext>& contexts,
    MetricsCollector& metrics,
    BatchProcessingHelpers& batch_helpers,  // ADD THIS
    std::atomic<bool>& cancelled)
    : config_(config)
    , gpu_contexts_(contexts)
    , metrics_(metrics)
    , batch_helpers_(batch_helpers)  // ADD THIS
    , cancelled_(cancelled)
    , d_fm_index_(nullptr)
    , d_reference_(nullptr)
    , reference_length_(0)
    , bam_writer_(nullptr)
    , estimated_total_reads_(0)
{
}
```

### Fix 3: Implement Alignment Building

**File**: `src/core/pipeline_multistream.cpp`

**Replace TODO with actual implementation:**
```cpp
void MultiStreamScheduler::process_context_results(GpuBatchContext& ctx) {
    using namespace std::chrono;
    auto write_start = high_resolution_clock::now();

    // Copy GPU results to host
    ctx.host_results.resize(ctx.num_seeds);
    if (ctx.num_seeds > 0 && ctx.pinned_results) {
        std::memcpy(ctx.host_results.data(), ctx.pinned_results,
                   ctx.num_seeds * sizeof(cuda::AlignmentResult));
    }

    // BUILD ALIGNMENTS AND WRITE TO BAM (NEW CODE)
    for (size_t i = 0; i < ctx.read_count; ++i) {
        const auto& view = ctx.read_views[i];

        // Find best alignment for this read
        cuda::AlignmentResult best_result;
        bool found = false;

        for (size_t j = 0; j < ctx.num_seeds; ++j) {
            if (ctx.host_results[j].read_id == i) {
                if (!found || ctx.host_results[j].score > best_result.score) {
                    best_result = ctx.host_results[j];
                    found = true;
                }
            }
        }

        Alignment aln;
        if (found && best_result.score > 0) {
            // Build alignment from GPU result
            aln = batch_helpers_.build_alignment_from_gpu_result(best_result, view);
            batch_helpers_.update_metrics(true, aln.mapq);
        } else {
            // Build unmapped alignment
            const Read& read = ctx.is_paired ? ctx.pairs[i/2].read1 : ctx.singles[i];
            bool is_second = ctx.is_paired && (i % 2 == 1);
            aln = batch_helpers_.build_unmapped_alignment(read, ctx.is_paired, is_second);
            batch_helpers_.update_metrics(false, 0);
        }

        // Write to BAM
        if (bam_writer_) {
            bam_writer_->write_alignment(aln);
        }
    }

    ctx.timing.t_write = duration_cast<microseconds>(
        high_resolution_clock::now() - write_start).count() / 1000.0;

    PerformanceCounters counters;
    counters.total_gpu_seeds = ctx.num_seeds;
    counters.batches_processed = 1;
    metrics_.update_counters(counters);

    ctx.timing.t_total = duration_cast<microseconds>(
        high_resolution_clock::now() - ctx.batch_start).count() / 1000.0;
    metrics_.record_batch(ctx.timing, metrics_.get_batch_count() + 1);
}
```

### Fix 4: Update Pipeline.cpp Integration

**File**: `src/core/pipeline.cpp`

**Update run_multi_stream() to pass batch_helpers:**
```cpp
Result<bool> Pipeline::Impl::run_multi_stream() {
    if (!running_) {
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
    }

    Logger::instance().info("Using multi-stream GPU scheduler");

    // Create multi-stream scheduler
    scheduler_ = std::make_unique<internal::MultiStreamScheduler>(
        config_,
        initializer_->get_gpu_context_manager()->get_contexts(),
        *metrics_,
        *batch_helpers_,  // ADD THIS LINE
        cancelled_
    );

    // Initialize scheduler with all required resources
    uint64_t estimated_reads = estimate_total_reads();
    scheduler_->initialize(
        initializer_->get_fm_index_view(),
        initializer_->get_device_reference(),
        initializer_->get_reference_length(),
        initializer_->get_bam_writer(),
        estimated_reads,
        [this](double p, const std::string& m) { update_progress(p, m); }
    );

    // Run the multi-stream scheduler
    auto result = scheduler_->run();

    return result;
}
```

### Fix 5: Add Missing Include

**File**: `src/core/pipeline_multistream.h`

**Add forward declaration:**
```cpp
namespace winalign {

// Forward declarations
struct PipelineConfig;
class BamWriter;

namespace cuda {
struct FMIndexView;
}

namespace internal {

// Forward declaration
class MetricsCollector;
class BatchProcessingHelpers;  // ADD THIS

/**
 * @brief Multi-stream GPU scheduler with async pipeline stages
 */
class MultiStreamScheduler {
    // ...
};
```

**File**: `src/core/pipeline_multistream.cpp`

**Add include at top:**
```cpp
#include "pipeline_multistream.h"
#include "pipeline_metrics.h"
#include "pipeline_batch_helpers.h"  // ADD THIS
#include "winalign/logger.h"
// ... rest of includes
```

---

## ✅ Testing Checklist

After implementing fixes:

### Compilation
- [ ] Code compiles without errors
- [ ] All includes resolve correctly
- [ ] No linking errors

### Functionality
- [ ] Pipeline runs without crashes
- [ ] BAM file is created
- [ ] BAM file contains alignment records
- [ ] Alignment records have correct positions
- [ ] Unmapped reads are properly flagged
- [ ] Metrics show mapped/unmapped counts

### Performance
- [ ] No significant performance regression
- [ ] Multi-stream scheduling still works
- [ ] GPU utilization normal
- [ ] Memory usage acceptable

---

## 📊 Estimated Effort

| Task | Effort | Priority |
|------|--------|----------|
| Add BatchProcessingHelpers dependency | 15 min | HIGH |
| Implement alignment building logic | 45 min | HIGH |
| Update pipeline.cpp integration | 10 min | HIGH |
| Testing and validation | 30 min | HIGH |
| **TOTAL** | **~1.5 hours** | **HIGH** |

---

## 🎯 Implementation Order

1. ✅ **First**: Update `pipeline_multistream.h` (add dependency)
2. ✅ **Second**: Update `pipeline_multistream.cpp` constructor
3. ✅ **Third**: Implement `process_context_results()` alignment building
4. ✅ **Fourth**: Update `pipeline.cpp` to pass batch_helpers
5. ✅ **Fifth**: Test compilation
6. ✅ **Sixth**: Test with real data

---

## 📝 Additional Notes

### Why This Was Missed

The refactoring correctly extracted the alignment building logic into `BatchProcessingHelpers`, but the integration step didn't wire up the dependency to `MultiStreamScheduler`. This is a **missing dependency injection** issue.

### Why Code Still Compiles

The code compiles because:
- All syntax is correct
- No undefined references (TODO is just a comment)
- The warning message doesn't cause errors
- The pipeline "runs" but produces no output

### Lesson Learned

When extracting modules:
1. ✅ Extract logic ✓ (Done correctly)
2. ✅ Create module classes ✓ (Done correctly)
3. ❌ **Wire up all dependencies** ✗ (MISSED THIS STEP)
4. ❌ **Test end-to-end** ✗ (Would have caught this)

---

## 🔗 Related Files

- `src/core/pipeline_multistream.{h,cpp}` - Needs BatchProcessingHelpers
- `src/core/pipeline_batch_helpers.{h,cpp}` - Provides alignment building
- `src/core/pipeline.cpp` - Needs to pass batch_helpers to scheduler
- `docs/INTEGRATION_PLAN.md` - Documents the integration strategy

---

**Status**: 🔴 **BLOCKING** - Must fix before code is functional
**Next Action**: Implement fixes 1-5 in order
**Estimated Time**: 1.5 hours
**Priority**: IMMEDIATE
