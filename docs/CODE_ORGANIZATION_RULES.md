# WinAlign Code Organization Rules

**Version**: 1.0
**Date**: 2025-11-19
**Status**: ⚠️ MANDATORY - All new code must follow these rules

---

## 🚨 HARD LIMITS - NO EXCEPTIONS

### File Size Limits

| File Type | Hard Limit | Target | Action if Exceeded |
|-----------|-----------|--------|-------------------|
| **Source files** (.cpp, .cu, .c) | **500 lines** | 300 lines | MUST refactor before merge |
| **Header files** (.h, .cuh, .hpp) | **300 lines** | 200 lines | MUST refactor before merge |
| **Test files** (_test.cpp) | **400 lines** | 250 lines | MUST refactor before merge |
| **Utility files** (tools/*.cpp) | **400 lines** | 250 lines | MUST refactor before merge |

### Function Complexity Limits

| Metric | Hard Limit | Target | Action if Exceeded |
|--------|-----------|--------|-------------------|
| **Lines per function** | **100 lines** | 50 lines | MUST break into smaller functions |
| **Cyclomatic complexity** | **15** | 10 | MUST simplify logic |
| **Nesting depth** | **4 levels** | 3 levels | MUST flatten or extract |
| **Parameters per function** | **8** | 5 | MUST use structs/objects |

### CUDA-Specific Limits

| Metric | Hard Limit | Reasoning |
|--------|-----------|-----------|
| **Kernel lines** | **150 lines** | Readability, occupancy analysis |
| **Shared memory per kernel** | **48 KB** | Hardware limit (most GPUs) |
| **Registers per thread** | **64** | Occupancy optimization |
| **Device functions** | **50 lines** | Inlining, compile time |

---

## 📊 Current Violations (MUST FIX)

### Critical - Immediate Refactoring Required

```
❌ src/core/pipeline.cpp             2,288 lines  (LIMIT: 500)  → 358% over limit
❌ src/cuda/filtering.cu                649 lines  (LIMIT: 500)  → 30% over limit
❌ src/cuda/alignment.cu                566 lines  (LIMIT: 500)  → 13% over limit
❌ src/cuda/seeding.cu                  565 lines  (LIMIT: 500)  → 13% over limit
```

### High Priority - Refactoring Needed

```
⚠️ src/cpu/fm_index.cpp                477 lines  (TARGET: 300)  → Approaching limit
⚠️ src/amplicon/performance_monitor.cpp 437 lines  (TARGET: 300)  → Approaching limit
⚠️ src/amplicon/realtime_processor.cpp  417 lines  (TARGET: 300)  → Approaching limit
⚠️ src/amplicon/barcode_demux.cpp       391 lines  (TARGET: 300)  → Approaching limit
```

---

## 🔧 Refactoring Strategy

### Phase 1: Emergency Refactoring (This Week)

**Priority 1: pipeline.cpp (2,288 → 500 lines max)**

Break into modular files:
```
src/core/
├── pipeline.cpp                 (300 lines) - Main orchestration
├── pipeline_initialization.cpp  (250 lines) - Setup/teardown
├── pipeline_gpu_context.cpp     (300 lines) - GPU context management
├── pipeline_multistream.cpp     (400 lines) - Multi-stream scheduler
├── pipeline_batch_processing.cpp (300 lines) - Batch processing logic
├── pipeline_metrics.cpp          (250 lines) - Performance metrics
└── pipeline_internal.h           (200 lines) - Shared internal definitions
```

**Priority 2: CUDA Kernels (649 → 500 lines max)**

**filtering.cu breakdown:**
```
src/cuda/
├── filtering.cu                  (200 lines) - Main API + simple filters
├── filtering_quality.cu          (150 lines) - Quality filtering kernels
├── filtering_duplicates.cu       (150 lines) - Duplicate marking kernels
├── filtering_pairs.cu            (150 lines) - Paired-end validation
└── filtering_internal.cuh        (100 lines) - Shared device functions
```

**alignment.cu breakdown:**
```
src/cuda/
├── alignment.cu                  (200 lines) - Main API + host functions
├── alignment_banded_warp.cu      (200 lines) - Warp-optimized kernel
├── alignment_legacy.cu           (150 lines) - Fallback kernel
└── alignment_device.cuh          (100 lines) - Device helper functions
```

**seeding.cu breakdown:**
```
src/cuda/
├── seeding.cu                    (200 lines) - Main API + host functions
├── seeding_optimized.cu          (200 lines) - Optimized FM-index kernel
├── seeding_legacy.cu             (150 lines) - Basic seeding kernel
└── seeding_device.cuh            (100 lines) - Device helper functions
```

### Phase 2: Preventive Measures (Ongoing)

1. **Pre-commit hooks** - Block commits with files > 500 lines
2. **CI checks** - Fail builds if any file exceeds limits
3. **Code review checklist** - Require refactoring plan for 300+ line changes
4. **Automated alerts** - Warning when file approaches 400 lines

---

## 📋 Refactoring Rules

### Rule 1: Single Responsibility Principle
```
✅ GOOD: One file = One clear purpose
❌ BAD:  One file = Multiple unrelated features
```

**Example:**
```cpp
// ❌ BAD: pipeline.cpp doing everything
class Pipeline {
    void run();                    // Orchestration
    void initialize_gpu();         // GPU setup
    void process_batch();          // Batch processing
    void write_metrics();          // Logging
    void cleanup_resources();      // Cleanup
    // ... 2000 more lines
};

// ✅ GOOD: Separate concerns
// pipeline.cpp
class Pipeline {
    void run();  // Uses other classes
private:
    GpuContextManager gpu_;
    BatchProcessor processor_;
    MetricsCollector metrics_;
};

// pipeline_gpu_context.cpp
class GpuContextManager { ... };

// pipeline_batch_processing.cpp
class BatchProcessor { ... };

// pipeline_metrics.cpp
class MetricsCollector { ... };
```

### Rule 2: Extract Common Patterns

**CUDA Kernels:**
```cuda
// ❌ BAD: Inline repeated logic
__global__ void kernel_a(...) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n) return;
    // ... repeated bounds checking ...
    // ... repeated memory coalescing ...
}

__global__ void kernel_b(...) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    if (tid >= n) return;
    // ... same repeated logic ...
}

// ✅ GOOD: Extract to device functions
// device_common.cuh
__device__ inline bool check_bounds(uint32_t tid, uint32_t n) {
    return tid < n;
}

__device__ inline uint32_t get_global_tid() {
    return blockIdx.x * blockDim.x + threadIdx.x;
}

// kernel_a.cu
__global__ void kernel_a(...) {
    uint32_t tid = get_global_tid();
    if (!check_bounds(tid, n)) return;
    // ... unique logic only ...
}
```

### Rule 3: File Naming Convention

```
<module>_<component>_<variant>.{cpp|cu}

Examples:
✅ pipeline_multistream.cpp          - Clear module + component
✅ alignment_banded_warp.cu          - Clear variant
✅ seeding_optimized.cu              - Clear optimization variant
✅ filtering_quality.cu              - Clear filter type

❌ pipeline2.cpp                     - Unclear naming
❌ alignment_new.cu                  - Vague naming
❌ utils.cpp                         - Too generic
```

### Rule 4: Header Organization

```cpp
// ✅ GOOD: Forward declarations in headers
// pipeline.h
namespace winalign {
    class GpuContextManager;  // Forward declare
    class BatchProcessor;     // Forward declare

    class Pipeline {
        // Public API only
        Result<bool> run();
    private:
        std::unique_ptr<GpuContextManager> gpu_;
        std::unique_ptr<BatchProcessor> processor_;
    };
}

// ❌ BAD: Full definitions in headers
// pipeline.h (1000 lines)
namespace winalign {
    class GpuContextManager {
        // Entire implementation in header
        void initialize() { /* 200 lines */ }
        void cleanup() { /* 150 lines */ }
    };
}
```

---

## 🛠️ Refactoring Process

### Step 1: Analyze File

```bash
# Check file size
wc -l src/core/pipeline.cpp

# Identify logical sections
grep -n "^//" src/core/pipeline.cpp | grep "==="

# Find function boundaries
ctags -x --c++-kinds=f src/core/pipeline.cpp
```

### Step 2: Create Extraction Plan

```markdown
File: pipeline.cpp (2288 lines)

Sections to extract:
1. GPU Context Management (lines 850-1300) → pipeline_gpu_context.cpp
2. Multi-stream scheduler (lines 372-642) → pipeline_multistream.cpp
3. Batch processing (lines 650-850) → pipeline_batch_processing.cpp
4. Performance metrics (lines 700-850) → pipeline_metrics.cpp
5. Initialization (lines 200-370) → pipeline_initialization.cpp
```

### Step 3: Extract Incrementally

```bash
# Create new file
touch src/core/pipeline_gpu_context.cpp

# Move code (DON'T use sed for large operations)
# Instead, manually copy sections in editor

# Update CMakeLists.txt
# Add to winalign_core target

# Build and test
cmake --build build --target winalign_core

# Verify no regressions
./build/bin/winalign-test
```

### Step 4: Update Documentation

```markdown
# Update IMPLEMENTATION_PROGRESS.md
- Document new file structure
- Update line references
- Note any API changes
```

---

## 🚫 Anti-Patterns to Avoid

### Anti-Pattern 1: God Objects

```cpp
// ❌ BAD: 2000-line class doing everything
class Pipeline {
    // Orchestration
    void run();

    // GPU management
    void initialize_gpu_contexts();
    void cleanup_gpu_contexts();
    void launch_h2d_transfer();
    void launch_seeding();
    void launch_alignment();
    void launch_d2h_transfer();

    // Batch processing
    void load_fastq_batch();
    void prepare_context();
    void process_results();

    // Metrics
    void log_batch_timing();
    void log_performance_summary();

    // ... 50 more methods
    // ... 100 private members
};
```

**Solution:** Split into focused classes with single responsibilities

### Anti-Pattern 2: Mega Functions

```cpp
// ❌ BAD: 800-line function
void Pipeline::run_multi_stream() {
    // 800 lines of tangled logic
    // Impossible to test
    // Impossible to understand
}

// ✅ GOOD: Composed from smaller functions
void Pipeline::run_multi_stream() {
    initialize_worker();
    while (has_work()) {
        process_load_stage();
        process_h2d_stage();
        process_compute_stage();
        process_d2h_stage();
        process_write_stage();
    }
    finalize_worker();
}

// Each sub-function is 20-50 lines, testable, clear
```

### Anti-Pattern 3: Kitchen Sink Headers

```cpp
// ❌ BAD: Everything in one header
// cuda_kernels.cuh (1500 lines)
__global__ void seeding_kernel(...);
__global__ void alignment_kernel(...);
__global__ void filtering_kernel(...);
// ... 50 more kernels
// ... 100 device functions
// ... All in one file

// ✅ GOOD: Separate headers by module
// cuda/seeding.cuh
__global__ void fm_seed_kernel(...);
__global__ void fm_seed_kernel_optimized(...);

// cuda/alignment.cuh
__global__ void smith_waterman_banded_warp_kernel(...);

// cuda/filtering.cuh
__global__ void filter_quality_kernel(...);
```

---

## ✅ Refactoring Checklist

Before merging any refactored code:

- [ ] All files under 500 lines (hard limit)
- [ ] Target of <300 lines achieved where possible
- [ ] All functions under 100 lines
- [ ] No function has cyclomatic complexity > 15
- [ ] Tests pass (no regressions)
- [ ] Build time not significantly increased
- [ ] Documentation updated (line number references)
- [ ] CMakeLists.txt updated correctly
- [ ] No duplicate code introduced
- [ ] Clear naming convention followed
- [ ] Header dependencies minimized (forward declarations)
- [ ] CUDA kernels follow occupancy best practices

---

## 📏 Measurement & Enforcement

### Pre-commit Hook

```bash
#!/bin/bash
# .git/hooks/pre-commit

MAX_LINES=500
VIOLATIONS=0

for file in $(git diff --cached --name-only --diff-filter=AM | grep -E '\.(cpp|cu|c|h|cuh|hpp)$'); do
    if [ -f "$file" ]; then
        LINES=$(wc -l < "$file")
        if [ $LINES -gt $MAX_LINES ]; then
            echo "❌ BLOCKED: $file has $LINES lines (limit: $MAX_LINES)"
            VIOLATIONS=$((VIOLATIONS + 1))
        fi
    fi
done

if [ $VIOLATIONS -gt 0 ]; then
    echo ""
    echo "Commit blocked: $VIOLATIONS file(s) exceed 500-line limit"
    echo "Please refactor before committing. See docs/CODE_ORGANIZATION_RULES.md"
    exit 1
fi

exit 0
```

### CI Check (GitHub Actions / GitLab CI)

```yaml
# .github/workflows/code-quality.yml
name: Code Quality Checks

on: [push, pull_request]

jobs:
  check-file-size:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Check file size limits
        run: |
          MAX_LINES=500
          VIOLATIONS=0

          while IFS= read -r file; do
            LINES=$(wc -l < "$file")
            if [ $LINES -gt $MAX_LINES ]; then
              echo "❌ $file: $LINES lines (limit: $MAX_LINES)"
              VIOLATIONS=$((VIOLATIONS + 1))
            fi
          done < <(find src -type f \( -name "*.cpp" -o -name "*.cu" -o -name "*.c" \))

          if [ $VIOLATIONS -gt 0 ]; then
            echo "❌ Build failed: $VIOLATIONS file(s) exceed limits"
            exit 1
          fi

          echo "✅ All files within size limits"
```

---

## 🎯 Immediate Action Items

### Week 1: Critical Files

1. **pipeline.cpp** (2,288 lines)
   - Target: Split into 5-6 files
   - Owner: TBD
   - Deadline: 2025-11-26

2. **filtering.cu** (649 lines)
   - Target: Split into 4 files
   - Owner: TBD
   - Deadline: 2025-11-26

### Week 2: High Priority Files

3. **alignment.cu** (566 lines)
   - Target: Split into 3 files
   - Owner: TBD
   - Deadline: 2025-12-03

4. **seeding.cu** (565 lines)
   - Target: Split into 3 files
   - Owner: TBD
   - Deadline: 2025-12-03

### Week 3: Enforcement

5. Install pre-commit hooks on all dev machines
6. Enable CI checks in pipeline
7. Update contributor documentation

---

## 📚 References

- **Clean Code** by Robert C. Martin - Single Responsibility Principle
- **CUDA Best Practices Guide** - Kernel organization
- **Google C++ Style Guide** - File organization
- **Linux Kernel Coding Style** - Function size limits

---

**Approved by**: TBD
**Last Reviewed**: 2025-11-19
**Next Review**: 2025-12-01

**This is a living document. All developers must follow these rules. No exceptions.**
