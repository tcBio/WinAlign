# WinAlign CUDA Kernel Refactoring - Complete Overview

**Date**: 2025-11-19
**Status**: 📋 ALL PLANS COMPLETE - Ready to execute
**Week**: Week 2 (CUDA Kernels)
**Duration**: Est. 5-7 days

---

## 🎯 Executive Summary

Complete refactoring plan for 3 CUDA kernel files that violate the 500-line limit:
- `filtering.cu`: 649 lines → 5 files
- `alignment.cu`: 566 lines → 4 files
- `seeding.cu`: 565 lines → 4 files

**Total**: 1,780 lines → 13 new files, all compliant

---

## 📊 Current Violations

### Critical Files

| File | Current Lines | Over Limit | Severity | Plan |
|------|--------------|------------|----------|------|
| filtering.cu | 649 | +149 (30%) | HIGH | 5 files |
| alignment.cu | 566 | +66 (13%) | MEDIUM | 4 files |
| seeding.cu | 565 | +65 (13%) | MEDIUM | 4 files |
| **TOTAL** | **1,780** | **+280** | - | **13 files** |

---

## 🗂️ Refactoring Breakdown

### 1. filtering.cu (649 → 5 files)

**Modules:**
- **filtering_device.cuh** (~100 lines)
  - SAM flags, device functions, functors
  - Shared by all filtering modules

- **filtering_quality.cu** (~180 lines)
  - filter_quality_kernel
  - filter_by_quality host function
  - CUB reduction for counting

- **filtering_duplicates.cu** (~150 lines)
  - mark_duplicates_kernel
  - mark_duplicates host function
  - compact_results (thrust::remove_if)

- **filtering_pairs.cu** (~150 lines)
  - validate_pairs_kernel
  - validate_pairs host function

- **filtering.cu** (~200 lines)
  - compute_stats_kernel
  - sort_by_coordinate (thrust::sort)
  - compute_statistics
  - Main API glue

**Estimated Total**: ~780 lines (distributed)
**All Files**: Under 500-line limit ✅

---

### 2. alignment.cu (566 → 4 files)

**Modules:**
- **alignment_device.cuh** (~100 lines)
  - match_score, max3 device functions
  - band_index helper
  - Shared constants

- **alignment_banded_warp.cu** (~250 lines)
  - smith_waterman_banded_warp_kernel (Phase 3)
  - smith_waterman_align_banded_warp host
  - Warp-optimized implementation

- **alignment_legacy.cu** (~200 lines)
  - smith_waterman_kernel (fallback)
  - generate_cigar_kernel
  - calculate_mapq_kernel

- **alignment.cu** (~200 lines)
  - smith_waterman_align (auto-select)
  - batch_smith_waterman
  - Memory management
  - Helper function wrappers

**Estimated Total**: ~750 lines (distributed)
**All Files**: Under 500-line limit ✅

---

### 3. seeding.cu (565 → 4 files)

**Modules:**
- **seeding_device.cuh** (~150 lines)
  - char_to_2bit, char_to_base
  - occ_rank (basic)
  - backward_search
  - Shared FM-index operations

- **seeding_basic.cu** (~180 lines)
  - fm_seed_kernel (basic)
  - compact_seeds_kernel
  - generate_gpu_seeds_basic host

- **seeding_optimized.cu** (~250 lines)
  - occ_rank_cached (Phase 4)
  - fm_seed_kernel_optimized (shared mem BWT)
  - generate_gpu_seeds_optimized host

- **seeding.cu** (~180 lines)
  - generate_gpu_seeds (auto-select)
  - Memory management (allocate/free)
  - copy_fm_index_to_device

**Estimated Total**: ~760 lines (distributed)
**All Files**: Under 500-line limit ✅

---

## 📋 Complete File Manifest

### New Header Files (3)
```
src/cuda/filtering_device.cuh    (~100 lines)
src/cuda/alignment_device.cuh    (~100 lines)
src/cuda/seeding_device.cuh      (~150 lines)
```

### New CUDA Source Files (10)
```
src/cuda/filtering_quality.cu     (~180 lines)
src/cuda/filtering_duplicates.cu  (~150 lines)
src/cuda/filtering_pairs.cu       (~150 lines)

src/cuda/alignment_banded_warp.cu (~250 lines)
src/cuda/alignment_legacy.cu      (~200 lines)

src/cuda/seeding_basic.cu         (~180 lines)
src/cuda/seeding_optimized.cu     (~250 lines)
```

### Modified Main Files (3)
```
src/cuda/filtering.cu  (649 → ~200 lines, -449)
src/cuda/alignment.cu  (566 → ~200 lines, -366)
src/cuda/seeding.cu    (565 → ~180 lines, -385)
```

### Total Impact
- **New files**: 13 (3 headers + 10 source)
- **Modified files**: 3 + CMakeLists.txt
- **Lines added**: ~2,290 (in new files)
- **Lines removed**: ~1,200 (from main files)
- **Net change**: +1,090 lines (due to file separation overhead)
- **Compliance**: 100% (all files under 500 lines)

---

## 🔗 Dependencies & Build

### CMakeLists.txt Updates

**Before:**
```cmake
cuda_add_library(winalign_cuda
    filtering.cu
    alignment.cu
    seeding.cu
    memory_manager.cu
)
```

**After:**
```cmake
cuda_add_library(winalign_cuda
    # Filtering modules
    filtering.cu
    filtering_quality.cu
    filtering_duplicates.cu
    filtering_pairs.cu

    # Alignment modules
    alignment.cu
    alignment_banded_warp.cu
    alignment_legacy.cu

    # Seeding modules
    seeding.cu
    seeding_basic.cu
    seeding_optimized.cu

    # Other
    memory_manager.cu
)
```

### Include Hierarchy
```
Public Headers (unchanged):
  winalign/cuda/filtering.cuh
  winalign/cuda/alignment.cuh
  winalign/cuda/seeding.cuh
      ↓
Private Device Headers (new):
  src/cuda/filtering_device.cuh
  src/cuda/alignment_device.cuh
  src/cuda/seeding_device.cuh
      ↓
Implementation Files:
  src/cuda/*.cu (all modules)
```

**Key Point**: Public API headers remain unchanged - all changes are internal implementation details.

---

## 🎯 Execution Strategy

### Phase 1: Preparation (Day 1)
1. ✅ Create all 3 refactoring plans (COMPLETE)
2. ✅ Review and validate plans
3. Create CUDA_REFACTORING_OVERVIEW.md (this document)
4. Create tracking document for execution

### Phase 2: Extraction (Days 2-4)
Execute extractions in order of complexity:

**Day 2: filtering.cu (5 files)**
- Create filtering_device.cuh
- Create filtering_quality.cu
- Create filtering_duplicates.cu
- Create filtering_pairs.cu
- Update filtering.cu
- Estimated: 6-8 hours

**Day 3: alignment.cu (4 files)**
- Create alignment_device.cuh
- Create alignment_banded_warp.cu
- Create alignment_legacy.cu
- Update alignment.cu
- Estimated: 4-6 hours

**Day 4: seeding.cu (4 files)**
- Create seeding_device.cuh
- Create seeding_basic.cu
- Create seeding_optimized.cu
- Update seeding.cu
- Estimated: 4-6 hours

### Phase 3: Integration (Day 5)
1. Update src/cuda/CMakeLists.txt
2. Test compilation of all modules
3. Fix any compilation errors
4. Verify all new files are under limits

### Phase 4: Testing (Day 6)
1. Run CUDA unit tests
2. Run integration tests
3. Performance benchmarks
4. Validate output correctness

### Phase 5: Documentation (Day 7)
1. Update all line references in docs
2. Create completion summary
3. Update CODE_ORGANIZATION_RULES.md
4. Tag release

---

## ✅ Success Criteria

### Code Quality
- [ ] All 13 new files created
- [ ] All files under 500-line limit (headers under 300)
- [ ] Clear module boundaries
- [ ] Shared code properly extracted
- [ ] No code duplication

### Functionality
- [ ] Code compiles without errors
- [ ] All existing tests pass
- [ ] No feature regressions
- [ ] Output identical to pre-refactor

### Performance
- [ ] Within ±5% of baseline
- [ ] Phase 3/4 optimizations preserved
- [ ] No CUDA errors
- [ ] GPU utilization maintained

### Documentation
- [ ] All plans documented
- [ ] Line references updated
- [ ] API docs current
- [ ] CMakeLists.txt correct

---

## 🎓 Lessons from Pipeline.cpp

### What Worked Well
1. **Complete planning upfront** - All 3 plans before execution
2. **Clear separation of concerns** - Each module has one purpose
3. **Shared code extraction** - Device headers for common utilities
4. **Incremental testing** - Test each module as created
5. **Comprehensive documentation** - Easy to track and execute

### Apply to CUDA Refactoring
1. ✅ **Plan all 3 files first** - Done
2. ✅ **Extract shared code** - device.cuh files
3. ✅ **Clear naming** - module_variant.cu pattern
4. ⏳ **Test incrementally** - After each file
5. ⏳ **Document thoroughly** - Track progress

---

## 📊 Compliance Metrics

### Before Refactoring
```
❌ filtering.cu:  649 lines (30% over limit)
❌ alignment.cu:  566 lines (13% over limit)
❌ seeding.cu:    565 lines (13% over limit)
───────────────────────────────────────────
Total violations: 3 files, 280 lines over
Compliance rate:  0/3 (0%)
```

### After Refactoring
```
✅ filtering_device.cuh:      100 lines (33% of limit)
✅ filtering_quality.cu:      180 lines (36% of limit)
✅ filtering_duplicates.cu:   150 lines (30% of limit)
✅ filtering_pairs.cu:        150 lines (30% of limit)
✅ filtering.cu:              200 lines (40% of limit)

✅ alignment_device.cuh:      100 lines (33% of limit)
✅ alignment_banded_warp.cu:  250 lines (50% of limit)
✅ alignment_legacy.cu:       200 lines (40% of limit)
✅ alignment.cu:              200 lines (40% of limit)

✅ seeding_device.cuh:        150 lines (50% of limit)
✅ seeding_basic.cu:          180 lines (36% of limit)
✅ seeding_optimized.cu:      250 lines (50% of limit)
✅ seeding.cu:                180 lines (36% of limit)
───────────────────────────────────────────
Total files: 13 new files
Compliance rate: 13/13 (100%)
```

**Result**: 100% compliance, 0 violations ✅

---

## 🚀 Next Steps

### Immediate (Today)
1. ✅ Review this overview document
2. ✅ Commit all planning documents
3. Create execution tracking document
4. Get approval to proceed

### This Week (Days 2-7)
1. Execute filtering.cu extraction (Day 2)
2. Execute alignment.cu extraction (Day 3)
3. Execute seeding.cu extraction (Day 4)
4. Integration and testing (Days 5-6)
5. Documentation and release (Day 7)

### After Week 2
1. Monitor for any issues
2. Performance validation
3. Consider other files approaching limits
4. Setup enforcement (pre-commit hooks, CI)

---

## 📁 Related Documents

- **Detailed Plans**:
  - docs/REFACTORING_PLAN_FILTERING.md
  - docs/REFACTORING_PLAN_ALIGNMENT.md
  - docs/REFACTORING_PLAN_SEEDING.md

- **Background**:
  - docs/CODE_ORGANIZATION_RULES.md
  - docs/REFACTORING_ROADMAP.md
  - docs/REFACTORING_STATUS.md

- **Pipeline Work** (completed):
  - docs/PIPELINE_REFACTORING_SUMMARY.md
  - docs/REFACTORING_COMPLETE_SUMMARY.md
  - docs/TESTING_SUMMARY.md

---

## 🎉 Expected Outcome

After completing Week 2 CUDA refactoring:

✅ **All critical violations resolved** (3/3 files compliant)
✅ **13 new modular files** created
✅ **100% compliance** with 500-line limit
✅ **Phase 3/4 optimizations** preserved
✅ **No functionality lost**
✅ **Maintainable codebase** for future development

**Combined with Week 1 (pipeline.cpp):**
- **4 critical violations fixed** (pipeline.cpp + 3 CUDA files)
- **24 total modules created** (11 pipeline + 13 CUDA)
- **Reduction**: 4,068 → distributed modular architecture
- **Code quality**: Dramatically improved

---

**Status**: 📋 **PLANS COMPLETE** - Ready to execute Week 2
**Confidence**: HIGH (95%) - Proven pattern from Week 1
**Next Action**: Begin Day 2 execution (filtering.cu extraction)
**Owner**: Development Team
**Last Updated**: 2025-11-19
