# Pipeline.cpp Integration Plan (Step 8)

**Objective**: Refactor Pipeline::Impl to use extracted modules
**Target**: Reduce pipeline.cpp from 2,288 → ~300-400 lines
**Status**: 🔄 In Progress

---

## Current Structure (2,288 lines)

### Pipeline::Impl Class
```cpp
class Pipeline::Impl {
    // 80+ member variables
    const PipelineConfig& config_;
    std::unique_ptr<ReferenceLoader> reference_loader_;
    std::unique_ptr<BamWriter> bam_writer_;
    std::unique_ptr<cuda::MemoryManager> gpu_mem_manager_;

    // State
    std::atomic<bool> running_;
    std::atomic<bool> cancelled_;

    // GPU resources (30+ variables)
    cuda::ReadBatch d_read_batch_;
    cuda::Seed* d_seeds_;
    cuda::AlignmentResult* d_results_;
    cuda::FMIndex d_fm_index_;
    char* d_reference_;
    cudaStream_t compute_stream_;
    // ... many more

    // Pinned memory (10+ variables)
    char* pinned_read_sequences_;
    uint32_t* pinned_read_offsets_;
    // ... more

    // GPU contexts
    std::vector<GpuBatchContext> gpu_contexts_;

    // Metrics
    BatchTiming cumulative_timing_;
    PerformanceCounters perf_counters_;
    cudaEvent_t event_h2d_start_;
    // ... many events

    // Reference data
    const cuda::FMIndexView* fm_index_view_;
    const std::string* concatenated_reference_;

    // Methods (2000+ lines)
    Result<bool> initialize();           // 282 lines
    Result<bool> run();                  // 150 lines
    Result<bool> run_multi_stream();     // 280 lines
    Result<bool> process_gpu_batch();    // 330 lines
    bool prepare_gpu_batch();            // 166 lines
    // ... many more
};
```

---

## New Structure (Target: ~350 lines)

### Simplified Pipeline::Impl
```cpp
class Pipeline::Impl {
public:
    Impl(const PipelineConfig& cfg) : config_(cfg) {}

    Result<bool> initialize();
    Result<bool> run();
    Result<bool> finalize();
    void cancel();
    void set_progress_callback(ProgressCallback callback);
    const PipelineMetrics& get_metrics() const;

private:
    // Configuration
    const PipelineConfig& config_;

    // Module instances (OWNS these)
    std::unique_ptr<internal::PipelineInitializer> initializer_;
    std::unique_ptr<internal::MultiStreamScheduler> scheduler_;
    std::unique_ptr<internal::BatchProcessingHelpers> batch_helpers_;
    std::unique_ptr<internal::MetricsCollector> metrics_;

    // State
    std::atomic<bool> running_{false};
    std::atomic<bool> cancelled_{false};
    ProgressCallback progress_callback_;

    // Metrics (for public API)
    PipelineMetrics public_metrics_;

    // Helper methods
    void update_progress(double progress, const std::string& message);
    uint64_t estimate_total_reads();
    Result<bool> run_single_stream();  // Fallback for non-GPU
};
```

---

## Integration Mapping

### 1. Initialization (282 lines → ~30 lines)

**Before**:
```cpp
Result<bool> Pipeline::Impl::initialize() {
    // 282 lines of:
    // - Load reference
    // - Build FM-index
    // - Initialize GPU
    // - Allocate device memory
    // - Copy FM-index to device
    // - Copy reference to device
    // - Initialize GPU contexts
    // - Create BAM writer
    // - Allocate pinned buffers
    // - Create CUDA events
}
```

**After**:
```cpp
Result<bool> Pipeline::Impl::initialize() {
    running_ = true;

    // Create initializer
    initializer_ = std::make_unique<internal::PipelineInitializer>(
        config_,
        [this](double p, const std::string& m) { update_progress(p, m); }
    );

    // Delegate to initializer
    auto result = initializer_->initialize();
    if (!result.is_ok()) {
        running_ = false;
        return result;
    }

    // Create metrics collector
    metrics_ = std::make_unique<internal::MetricsCollector>();
    if (initializer_->is_gpu_enabled()) {
        metrics_->initialize_cuda_events();
    }

    // Create batch helpers
    batch_helpers_ = std::make_unique<internal::BatchProcessingHelpers>(
        initializer_->get_reference_loader(),
        initializer_->get_bam_writer(),
        public_metrics_,
        mapping_quality_sum_
    );

    return Result<bool>(true);
}
```

**Savings**: 282 → 30 lines (**252 lines removed**)

---

### 2. Multi-Stream Execution (280 lines → ~40 lines)

**Before**:
```cpp
Result<bool> Pipeline::Impl::run_multi_stream() {
    // 280 lines of:
    // - Create FASTQ worker
    // - Main scheduler loop
    // - Load batches into contexts
    // - Launch H2D transfers
    // - Launch seeding
    // - Launch alignment
    // - Launch D2H transfers
    // - Process results
    // - State machine management
}
```

**After**:
```cpp
Result<bool> Pipeline::Impl::run_multi_stream() {
    if (!running_) {
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
    }

    // Create scheduler
    scheduler_ = std::make_unique<internal::MultiStreamScheduler>(
        config_,
        initializer_->get_gpu_context_manager()->get_contexts(),
        *metrics_,
        cancelled_
    );

    // Initialize scheduler with resources
    uint64_t estimated_reads = estimate_total_reads();
    scheduler_->initialize(
        initializer_->get_fm_index_view(),
        initializer_->get_device_reference(),
        initializer_->get_reference_length(),
        initializer_->get_bam_writer(),
        estimated_reads,
        [this](double p, const std::string& m) { update_progress(p, m); }
    );

    // Run scheduler
    return scheduler_->run();
}
```

**Savings**: 280 → 40 lines (**240 lines removed**)

---

### 3. GPU Batch Processing (330 lines → ~20 lines)

**Before**:
```cpp
Result<bool> Pipeline::Impl::process_gpu_batch() {
    // 330 lines of:
    // - Prepare batch
    // - H2D transfer with timing
    // - GPU seeding
    // - GPU alignment
    // - D2H transfer
    // - Build alignments
    // - Write to BAM
}
```

**After**:
```cpp
// This is now handled by MultiStreamScheduler internally
// No longer needed in Pipeline::Impl
```

**Savings**: 330 lines removed (handled by scheduler)

---

### 4. Batch Preparation (166 lines → delegated)

**Before**:
```cpp
bool Pipeline::Impl::prepare_gpu_batch() {
    // 166 lines of:
    // - Flatten reads
    // - Copy to pinned buffers
    // - Set up device batch
}
```

**After**:
```cpp
// Handled by MultiStreamScheduler::prepare_context_for_gpu()
// No longer in Pipeline::Impl
```

**Savings**: 166 lines removed (delegated to scheduler)

---

### 5. Alignment Building (Multiple functions → delegated)

**Before**:
```cpp
Alignment build_alignment_from_gpu_result() { /* 22 lines */ }
Alignment build_unmapped_alignment() { /* 15 lines */ }
Alignment align_read_cpu() { /* 33 lines */ }
void process_cpu_alignment() { /* 40 lines */ }
```

**After**:
```cpp
// All handled by BatchProcessingHelpers
// Accessed via batch_helpers_ member
```

**Savings**: 110 lines removed (delegated to batch_helpers)

---

### 6. GPU Context Management (206 lines → delegated)

**Before**:
```cpp
bool initialize_gpu_contexts() { /* 104 lines */ }
void cleanup_gpu_contexts() { /* 102 lines */ }
```

**After**:
```cpp
// Handled by GpuContextManager (via PipelineInitializer)
// Cleanup automatic via unique_ptr
```

**Savings**: 206 lines removed (delegated to gpu_context_manager)

---

### 7. Metrics & Performance (90 lines → ~20 lines)

**Before**:
```cpp
void log_batch_timing() { /* 20 lines */ }
void log_performance_summary() { /* 20 lines */ }
float get_cuda_event_time() { /* 5 lines */ }
// Plus event management (45 lines)
```

**After**:
```cpp
Result<bool> Pipeline::Impl::finalize() {
    update_progress(0.90, "Finalizing output");

    // Log performance summary
    if (metrics_) {
        metrics_->log_performance_summary();
    }

    update_progress(1.0, "Complete");
    return Result<bool>(true);
}
```

**Savings**: 90 → 20 lines (**70 lines removed**)

---

## Total Line Reduction

| Section | Before | After | Saved |
|---------|--------|-------|-------|
| Member variables | ~200 | ~50 | 150 |
| initialize() | 282 | 30 | 252 |
| run_multi_stream() | 280 | 40 | 240 |
| process_gpu_batch() | 330 | 0 | 330 |
| prepare_gpu_batch() | 166 | 0 | 166 |
| Alignment helpers | 110 | 0 | 110 |
| GPU context mgmt | 206 | 0 | 206 |
| Metrics & perf | 90 | 20 | 70 |
| Helper functions | ~400 | ~100 | 300 |
| **TOTAL** | **~2064** | **~240** | **~1824** |

**Remaining in pipeline.cpp**:
- Class declaration: ~50 lines
- Helper methods: ~100 lines
- Public API wrapper: ~50 lines
- Includes & namespace: ~50 lines
- **Total: ~240-300 lines** ✅

---

## Remaining Code

### What Stays in Pipeline::Impl

1. **Public API Methods** (~50 lines)
   - `initialize()` - Delegate to initializer
   - `run()` - Route to multi-stream or single-stream
   - `finalize()` - Delegate to metrics
   - `cancel()` - Set cancelled flag
   - `set_progress_callback()` - Store callback
   - `get_metrics()` - Return public metrics

2. **Helper Methods** (~100 lines)
   - `update_progress()` - Call progress callback
   - `estimate_total_reads()` - Parse FASTQ headers
   - `run_single_stream()` - CPU-only fallback path

3. **PIMPL Wrapper** (~50 lines)
   - `Pipeline::Pipeline()`
   - `Pipeline::~Pipeline()`
   - `Pipeline::initialize()`
   - `Pipeline::run()`
   - etc.

4. **Includes & Namespace** (~40 lines)
   - Include new module headers
   - Namespace declarations

---

## Implementation Steps

### Phase 1: Add Module Includes
```cpp
#include "pipeline_internal.h"
#include "pipeline_gpu_context.h"
#include "pipeline_metrics.h"
#include "pipeline_multistream.h"
#include "pipeline_batch_helpers.h"
#include "pipeline_initialization.h"
```

### Phase 2: Refactor Pipeline::Impl Class
- Replace 80+ members with 4 module pointers
- Simplify initialize() to delegate
- Simplify run() to route
- Remove all delegated methods

### Phase 3: Update PIMPL Wrapper
- Ensure public API still works
- Update any method signatures if needed

### Phase 4: Remove Dead Code
- Delete all extracted functions
- Clean up unused member variables
- Remove redundant includes

---

## Testing Strategy

### 1. Compilation Test
```bash
cd build
cmake ..
make winalign_core
```

### 2. Link Test
```bash
make winalign
```

### 3. Smoke Test
```bash
./winalign --help
```

### 4. Integration Test
```bash
./winalign align \
  --reference test/data/ref.fasta \
  --read1 test/data/reads_1.fastq \
  --output test/output.bam
```

---

## Risk Mitigation

### Risk 1: Missing Dependencies
**Mitigation**: Careful include management, forward declarations

### Risk 2: API Breakage
**Mitigation**: Keep public API identical, only change implementation

### Risk 3: Performance Regression
**Mitigation**: Benchmark before/after, profile hotspots

### Risk 4: State Management Issues
**Mitigation**: Clear ownership model, document lifetimes

---

## Success Criteria

- [ ] Pipeline.cpp reduced to <400 lines
- [ ] All modules properly integrated
- [ ] Code compiles without errors
- [ ] Existing tests pass
- [ ] No performance regression (±5%)
- [ ] Public API unchanged

---

**Next Action**: Implement refactored Pipeline::Impl class
**Estimated Effort**: 2-3 hours
**Status**: 🔄 Ready to begin
