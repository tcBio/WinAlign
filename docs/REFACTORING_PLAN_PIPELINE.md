# Pipeline.cpp Refactoring Plan

**Current**: 2,288 lines (358% over 500-line limit)
**Target**: 5-6 files, each <400 lines
**Priority**: 🔴 CRITICAL - Blocking all other work
**Deadline**: 2025-11-26

---

## 📊 Current Structure Analysis

```bash
File: src/core/pipeline.cpp (2,288 lines)

Line Ranges by Section:
================================================================================
Lines    1-100   : Includes, namespace, forward declarations
Lines  101-200   : Structures (BatchTiming, PerformanceCounters, GpuBatchContext)
Lines  201-370   : Pipeline::initialize() - Setup and initialization
Lines  371-650   : Pipeline::run_multi_stream() - Multi-stream scheduler ⚠️ 280 lines
Lines  651-700   : Pipeline::run() - Simple wrapper
Lines  701-790   : Logging functions (log_batch_timing, log_performance_summary)
Lines  791-850   : Pipeline::finalize() - Cleanup
Lines  851-1010  : initialize_gpu_contexts() - Context allocation
Lines 1011-1050  : cleanup_gpu_contexts() - Context cleanup
Lines 1051-1200  : load_fastq_into_context_from_worker() - Batch loading
Lines 1201-1350  : prepare_context_for_gpu() - Data preparation
Lines 1351-1450  : launch_h2d_transfer() - Host-to-device transfers
Lines 1451-1550  : launch_seeding() - Seeding kernel launch
Lines 1551-1650  : launch_alignment() - Alignment kernel launch
Lines 1651-1750  : launch_d2h_transfer() - Device-to-host transfers
Lines 1751-2000  : process_context_results() - Result processing
Lines 2001-2100  : process_gpu_batch() - Legacy single-stream (fallback)
Lines 2101-2288  : Helper functions, utility methods
```

## 🎯 Refactoring Strategy

### New File Structure

```
src/core/
├── pipeline.h                          (200 lines) - Public API
├── pipeline.cpp                        (300 lines) - Main orchestration
├── pipeline_internal.h                 (150 lines) - Internal shared definitions
├── pipeline_initialization.cpp         (300 lines) - Setup/teardown
├── pipeline_gpu_context.cpp            (350 lines) - GPU context management
├── pipeline_multistream.cpp            (450 lines) - Multi-stream scheduler
├── pipeline_batch_processing.cpp       (350 lines) - Batch processing logic
├── pipeline_metrics.cpp                (250 lines) - Performance metrics & logging
└── pipeline_legacy.cpp                 (200 lines) - Fallback single-stream mode
```

**Total**: ~2,550 lines across 8 files (avg 319 lines/file)
**All files under 500-line hard limit** ✅

---

## 📝 Detailed Extraction Plan

### File 1: pipeline_internal.h (150 lines)

**Purpose**: Shared internal definitions (not in public API)

**Contents**:
```cpp
// Internal structures used across multiple pipeline files
namespace winalign {
namespace internal {

// From lines 101-200 of current pipeline.cpp
struct BatchTiming {
    double t_read = 0.0;
    double t_h2d = 0.0;
    double t_gpu_seed = 0.0;
    double t_gpu_align = 0.0;
    double t_d2h = 0.0;
    double t_write = 0.0;
    double t_total = 0.0;
};

struct PerformanceCounters {
    uint64_t gpu_aligned_reads = 0;
    uint64_t unmapped_reads = 0;
    uint64_t reads_without_seeds = 0;
    uint64_t total_gpu_seeds = 0;
    uint64_t batches_processed = 0;

    double avg_seeds_per_read() const;
};

enum class ContextState {
    EMPTY,
    LOADING,
    READY,
    TRANSFERRING,
    SEEDING,
    ALIGNING,
    COPYING_BACK,
    DONE,
    PROCESSING
};

struct GpuBatchContext {
    ContextState state = ContextState::EMPTY;
    uint32_t context_id = 0;

    // Host data
    std::vector<ReadPair> host_pairs;
    std::vector<Read> host_singles;

    // Device buffers
    cuda::ReadBatch d_read_batch;
    cuda::Seed* d_seeds = nullptr;
    cuda::AlignmentResult* d_results = nullptr;

    // Pinned host buffers
    char* pinned_sequences = nullptr;
    uint32_t* pinned_offsets = nullptr;
    uint32_t* pinned_lengths = nullptr;
    cuda::AlignmentResult* pinned_results = nullptr;

    // CUDA resources
    cudaStream_t stream = 0;
    cudaEvent_t event_h2d_done = 0;
    cudaEvent_t event_seeding_done = 0;
    cudaEvent_t event_sw_done = 0;
    cudaEvent_t event_d2h_done = 0;

    // Timing
    BatchTiming timing;

    // Batch size tracking
    uint32_t num_reads = 0;
    bool is_paired = false;
};

// Forward declarations for internal classes
class GpuContextManager;
class MultiStreamScheduler;
class BatchProcessor;
class MetricsCollector;

} // namespace internal
} // namespace winalign
```

**Lines**: ~150
**Dependencies**: Only public headers from include/

---

### File 2: pipeline_gpu_context.cpp (350 lines)

**Purpose**: GPU context lifecycle management

**Contents**:
```cpp
// Lines 851-1050 from current pipeline.cpp

namespace winalign {
namespace internal {

class GpuContextManager {
public:
    GpuContextManager(const Config& config, const cuda::FMIndex& fm_index);
    ~GpuContextManager();

    // Initialize all GPU contexts
    Result<bool> initialize();

    // Cleanup all GPU contexts
    void cleanup();

    // Get contexts for scheduler
    std::vector<GpuBatchContext>& get_contexts() { return contexts_; }
    const std::vector<GpuBatchContext>& get_contexts() const { return contexts_; }

    // Check if contexts are available
    bool is_initialized() const { return initialized_; }

private:
    // Configuration
    const Config& config_;
    const cuda::FMIndex& fm_index_;

    // GPU contexts (typically 3)
    std::vector<GpuBatchContext> contexts_;
    bool initialized_ = false;

    // Helper methods
    Result<bool> allocate_context(GpuBatchContext& ctx, uint32_t context_id);
    void free_context(GpuBatchContext& ctx);

    // Timing events
    cudaEvent_t event_h2d_start_ = 0;
    cudaEvent_t event_h2d_done_ = 0;
    cudaEvent_t event_seeding_start_ = 0;
    cudaEvent_t event_seeding_done_ = 0;
    cudaEvent_t event_sw_start_ = 0;
    cudaEvent_t event_sw_done_ = 0;
    cudaEvent_t event_d2h_start_ = 0;
    cudaEvent_t event_d2h_done_ = 0;
};

// Implementation from lines 851-1050
GpuContextManager::GpuContextManager(const Config& config, const cuda::FMIndex& fm_index)
    : config_(config), fm_index_(fm_index) {}

Result<bool> GpuContextManager::initialize() {
    // Move initialization code here (lines 856-1010)
    // ...
}

void GpuContextManager::cleanup() {
    // Move cleanup code here (lines 1011-1050)
    // ...
}

} // namespace internal
} // namespace winalign
```

**Lines**: ~350
**Extracted from**: Lines 851-1050 of current pipeline.cpp
**Dependencies**: pipeline_internal.h, CUDA headers

---

### File 3: pipeline_multistream.cpp (450 lines)

**Purpose**: Multi-stream GPU scheduling logic

**Contents**:
```cpp
// Lines 371-650 from current pipeline.cpp

namespace winalign {
namespace internal {

class MultiStreamScheduler {
public:
    MultiStreamScheduler(
        const Config& config,
        GpuContextManager& gpu_manager,
        FastqWorker& fastq_worker,
        BamWriter& bam_writer,
        MetricsCollector& metrics
    );

    // Main scheduler loop
    Result<bool> run();

    // State transition handlers
    void process_load_stage();
    void process_h2d_stage();
    void process_seeding_stage();
    void process_alignment_stage();
    void process_d2h_stage();
    void process_write_stage();

private:
    // References to other components
    const Config& config_;
    GpuContextManager& gpu_manager_;
    FastqWorker& fastq_worker_;
    BamWriter& bam_writer_;
    MetricsCollector& metrics_;

    // Scheduler state
    bool cancelled_ = false;
    uint32_t batches_processed_ = 0;

    // Helper methods
    bool all_contexts_done() const;
    void check_cancellation();
    void update_progress();

    // Stage-specific helpers (from lines 1051-2000)
    Result<bool> load_fastq_into_context(GpuBatchContext& ctx);
    Result<bool> prepare_context_for_gpu(GpuBatchContext& ctx);
    Result<bool> launch_h2d_transfer(GpuBatchContext& ctx);
    Result<bool> launch_seeding(GpuBatchContext& ctx);
    Result<bool> launch_alignment(GpuBatchContext& ctx);
    Result<bool> launch_d2h_transfer(GpuBatchContext& ctx);
    Result<bool> process_context_results(GpuBatchContext& ctx);
};

// Implementation
Result<bool> MultiStreamScheduler::run() {
    // Main event loop (lines 371-650)
    while (!cancelled_ && !all_contexts_done()) {
        process_load_stage();
        process_h2d_stage();
        process_seeding_stage();
        process_alignment_stage();
        process_d2h_stage();
        process_write_stage();

        check_cancellation();
        update_progress();
    }

    return Result<bool>(true);
}

// Each stage implementation (lines 400-650)
void MultiStreamScheduler::process_load_stage() { /* ... */ }
void MultiStreamScheduler::process_h2d_stage() { /* ... */ }
// ... etc

// Helper implementations (lines 1051-2000)
Result<bool> MultiStreamScheduler::load_fastq_into_context(GpuBatchContext& ctx) { /* ... */ }
Result<bool> MultiStreamScheduler::prepare_context_for_gpu(GpuBatchContext& ctx) { /* ... */ }
// ... etc

} // namespace internal
} // namespace winalign
```

**Lines**: ~450 (280 from main loop + 170 from helpers)
**Extracted from**: Lines 371-650, 1051-2000
**Dependencies**: pipeline_internal.h, pipeline_gpu_context.cpp

---

### File 4: pipeline_batch_processing.cpp (350 lines)

**Purpose**: Batch-level data processing (legacy single-stream fallback)

**Contents**:
```cpp
// Lines 2001-2100 from current pipeline.cpp

namespace winalign {
namespace internal {

class BatchProcessor {
public:
    BatchProcessor(
        const Config& config,
        const cuda::FMIndex& fm_index,
        BamWriter& bam_writer,
        MetricsCollector& metrics
    );

    // Process single batch (legacy mode)
    Result<bool> process_batch(
        const std::vector<ReadPair>& pairs,
        const std::vector<Read>& singles,
        bool is_paired
    );

private:
    const Config& config_;
    const cuda::FMIndex& fm_index_;
    BamWriter& bam_writer_;
    MetricsCollector& metrics_;

    // CUDA resources (single-stream)
    cuda::ReadBatch d_read_batch_;
    cuda::Seed* d_seeds_ = nullptr;
    cuda::AlignmentResult* d_results_ = nullptr;

    // Helper methods
    Result<bool> flatten_reads_to_gpu(/* ... */);
    Result<bool> execute_gpu_pipeline(/* ... */);
    Result<bool> retrieve_results(/* ... */);
    void update_metrics(/* ... */);
};

// Implementation
Result<bool> BatchProcessor::process_batch(/* ... */) {
    // Single-stream batch processing (lines 2001-2100)
    // This is the fallback when multi-stream is unavailable
    // ...
}

} // namespace internal
} // namespace winalign
```

**Lines**: ~350
**Extracted from**: Lines 2001-2288
**Dependencies**: pipeline_internal.h, CUDA headers

---

### File 5: pipeline_metrics.cpp (250 lines)

**Purpose**: Performance metrics collection and logging

**Contents**:
```cpp
// Lines 701-790 from current pipeline.cpp

namespace winalign {
namespace internal {

class MetricsCollector {
public:
    MetricsCollector();

    // Update metrics
    void record_batch_timing(const BatchTiming& timing);
    void increment_aligned_reads(uint64_t count);
    void increment_unmapped_reads(uint64_t count);
    void add_gpu_seeds(uint64_t count);
    void record_reads_without_seeds(uint64_t count);

    // Logging
    void log_batch_timing(uint32_t batch_id, const BatchTiming& timing);
    void log_performance_summary() const;

    // Getters
    const PerformanceCounters& get_counters() const { return counters_; }
    double get_average_batch_time() const;

private:
    PerformanceCounters counters_;
    std::vector<BatchTiming> batch_timings_;

    // Cumulative timing
    double total_read_time_ = 0.0;
    double total_h2d_time_ = 0.0;
    double total_gpu_seed_time_ = 0.0;
    double total_gpu_align_time_ = 0.0;
    double total_d2h_time_ = 0.0;
    double total_write_time_ = 0.0;
    double total_batch_time_ = 0.0;
};

// Implementation
void MetricsCollector::log_batch_timing(uint32_t batch_id, const BatchTiming& timing) {
    // From lines 701-750
    // ...
}

void MetricsCollector::log_performance_summary() const {
    // From lines 751-790
    // ...
}

} // namespace internal
} // namespace winalign
```

**Lines**: ~250
**Extracted from**: Lines 701-790
**Dependencies**: pipeline_internal.h

---

### File 6: pipeline_initialization.cpp (300 lines)

**Purpose**: Pipeline setup and teardown

**Contents**:
```cpp
// Lines 201-370, 791-850 from current pipeline.cpp

namespace winalign {
namespace internal {

class PipelineInitializer {
public:
    PipelineInitializer(Pipeline::Impl& impl);

    // Initialization
    Result<bool> initialize();

    // Finalization
    Result<bool> finalize();

private:
    Pipeline::Impl& impl_;

    // Initialization steps
    Result<bool> load_reference();
    Result<bool> build_fm_index();
    Result<bool> initialize_cuda();
    Result<bool> copy_reference_to_gpu();
    Result<bool> initialize_output();
    Result<bool> setup_gpu_contexts();
    Result<bool> setup_fastq_worker();

    // Finalization steps
    void cleanup_gpu_contexts();
    void cleanup_cuda_resources();
    void close_output_files();
    void write_final_metrics();
};

// Implementation
Result<bool> PipelineInitializer::initialize() {
    // From lines 201-370
    // Sequential initialization steps
    // ...
}

Result<bool> PipelineInitializer::finalize() {
    // From lines 791-850
    // Cleanup and summary
    // ...
}

} // namespace internal
} // namespace winalign
```

**Lines**: ~300
**Extracted from**: Lines 201-370, 791-850
**Dependencies**: pipeline_internal.h, all other pipeline files

---

### File 7: pipeline.cpp (300 lines) - MAIN FILE

**Purpose**: Public API implementation and orchestration

**Contents**:
```cpp
// Minimal main file - delegates to internal components

#include "winalign/pipeline.h"
#include "pipeline_internal.h"
#include "pipeline_initialization.cpp"
#include "pipeline_gpu_context.cpp"
#include "pipeline_multistream.cpp"
#include "pipeline_batch_processing.cpp"
#include "pipeline_metrics.cpp"

namespace winalign {

// Pipeline::Impl now contains instances of internal classes
struct Pipeline::Impl {
    Config config;

    // Internal components
    std::unique_ptr<internal::GpuContextManager> gpu_manager;
    std::unique_ptr<internal::MultiStreamScheduler> scheduler;
    std::unique_ptr<internal::BatchProcessor> batch_processor;
    std::unique_ptr<internal::MetricsCollector> metrics;
    std::unique_ptr<internal::PipelineInitializer> initializer;

    // Data
    cuda::FMIndex fm_index;
    std::unique_ptr<FastqWorker> fastq_worker;
    std::unique_ptr<BamWriter> bam_writer;

    // State
    std::atomic<bool> cancelled{false};
    ProgressCallback progress_callback;
};

Pipeline::Pipeline(const Config& config)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = config;

    // Create internal components
    impl_->metrics = std::make_unique<internal::MetricsCollector>();
    impl_->initializer = std::make_unique<internal::PipelineInitializer>(*impl_);
}

Pipeline::~Pipeline() = default;

Result<bool> Pipeline::initialize() {
    return impl_->initializer->initialize();
}

Result<bool> Pipeline::run() {
    // Delegate to multi-stream scheduler or fallback to batch processor
    if (impl_->gpu_manager && impl_->gpu_manager->is_initialized()) {
        return impl_->scheduler->run();
    } else {
        // Fallback to single-stream processing
        return run_single_stream();
    }
}

Result<bool> Pipeline::run_single_stream() {
    // Use batch processor for legacy mode
    // Simple loop reading batches and calling batch_processor
    // ...
}

Result<bool> Pipeline::finalize() {
    return impl_->initializer->finalize();
}

void Pipeline::cancel() {
    impl_->cancelled = true;
}

void Pipeline::set_progress_callback(ProgressCallback callback) {
    impl_->progress_callback = std::move(callback);
}

} // namespace winalign
```

**Lines**: ~300
**Purpose**: Thin wrapper around internal components
**Dependencies**: All other pipeline files

---

## 🔄 Migration Steps

### Step 1: Create Infrastructure (Day 1)

```bash
# Create new files with headers/includes only
touch src/core/pipeline_internal.h
touch src/core/pipeline_initialization.cpp
touch src/core/pipeline_gpu_context.cpp
touch src/core/pipeline_multistream.cpp
touch src/core/pipeline_batch_processing.cpp
touch src/core/pipeline_metrics.cpp

# Update CMakeLists.txt
vim src/core/CMakeLists.txt
```

**CMakeLists.txt changes:**
```cmake
add_library(winalign_core OBJECT
    pipeline.cpp
    pipeline_initialization.cpp
    pipeline_gpu_context.cpp
    pipeline_multistream.cpp
    pipeline_batch_processing.cpp
    pipeline_metrics.cpp
)
```

### Step 2: Extract Structures (Day 1)

```bash
# Move structures to pipeline_internal.h
# Lines 101-200 from pipeline.cpp → pipeline_internal.h

# Build to verify no breakage
cmake --build build --target winalign_core
```

### Step 3: Extract GPU Context Manager (Day 2)

```bash
# Move lines 851-1050 to pipeline_gpu_context.cpp
# Create GpuContextManager class
# Update pipeline.cpp to use GpuContextManager

# Build and test
cmake --build build --target winalign_core
./build/bin/winalign-test
```

### Step 4: Extract Metrics (Day 2)

```bash
# Move lines 701-790 to pipeline_metrics.cpp
# Create MetricsCollector class
# Update all callers

# Build and test
cmake --build build --target winalign_core
./build/bin/winalign-test
```

### Step 5: Extract MultiStream Scheduler (Day 3-4)

```bash
# Move lines 371-650 and 1051-2000 to pipeline_multistream.cpp
# Create MultiStreamScheduler class
# This is the most complex extraction

# Build and test thoroughly
cmake --build build --target winalign_core
./build/bin/winalign-test
```

### Step 6: Extract Initialization/Finalization (Day 5)

```bash
# Move lines 201-370 and 791-850 to pipeline_initialization.cpp
# Create PipelineInitializer class

# Build and test
cmake --build build --target winalign_core
./build/bin/winalign-test
```

### Step 7: Extract Batch Processor (Day 5)

```bash
# Move lines 2001-2288 to pipeline_batch_processing.cpp
# Create BatchProcessor class for fallback mode

# Build and test
cmake --build build --target winalign_core
./build/bin/winalign-test
```

### Step 8: Clean Up Main File (Day 6)

```bash
# Reduce pipeline.cpp to thin wrapper
# Should be ~300 lines now

# Final build and comprehensive testing
cmake --build build --target all
./build/bin/winalign-test --verbose
```

### Step 9: Verification (Day 6)

```bash
# Verify all files are under limits
find src/core -name "*.cpp" -o -name "*.h" | xargs wc -l

# Should see:
# 200 pipeline_internal.h
# 300 pipeline.cpp
# 300 pipeline_initialization.cpp
# 350 pipeline_gpu_context.cpp
# 450 pipeline_multistream.cpp
# 350 pipeline_batch_processing.cpp
# 250 pipeline_metrics.cpp
```

---

## ✅ Success Criteria

- [ ] All files under 500-line hard limit
- [ ] No regressions in tests
- [ ] Build time not significantly increased
- [ ] Documentation updated with new structure
- [ ] Code review passed
- [ ] All CUDA tests pass
- [ ] Memory profiling shows no leaks
- [ ] Performance benchmarks unchanged (±5%)

---

## 🚧 Risks and Mitigation

### Risk 1: Compilation Dependencies

**Problem**: Circular dependencies between new files

**Mitigation**:
- Use forward declarations in headers
- Keep internal namespace isolated
- Use PIMPL pattern in public API

### Risk 2: Build Time Increase

**Problem**: More compilation units = longer builds

**Mitigation**:
- Use precompiled headers
- Minimize header dependencies
- Consider unity builds for CI

### Risk 3: Test Breakage

**Problem**: Refactoring introduces subtle bugs

**Mitigation**:
- Extract incrementally (one file at a time)
- Run full test suite after each extraction
- Add integration tests before refactoring
- Use valgrind to detect memory issues

---

**Timeline**: 6 days
**Estimated Effort**: 30-40 hours
**Priority**: 🔴 CRITICAL
**Blocking**: All other development until complete

**Start Date**: 2025-11-20
**Target Completion**: 2025-11-26
