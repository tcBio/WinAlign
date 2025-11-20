# WinAlign Refactoring Roadmap

**Status**: 🔴 IN PROGRESS
**Start Date**: 2025-11-19
**Target Completion**: 2025-12-10
**Owner**: Development Team

---

## 📋 Executive Summary

WinAlign has accumulated technical debt with 4 critical files exceeding size limits:
- `pipeline.cpp`: 2,288 lines (needs to become 6 files)
- `filtering.cu`: 649 lines (needs to become 4 files)
- `alignment.cu`: 566 lines (needs to become 3 files)
- `seeding.cu`: 565 lines (needs to become 3 files)

**Total**: 4,068 lines to refactor → 16 modular files

This roadmap provides week-by-week execution plan with daily tasks.

---

## 🎯 Objectives

### Primary Goals
1. ✅ All files under 500-line hard limit
2. ✅ Target <300 lines per file where possible
3. ✅ Zero regressions in functionality
4. ✅ No performance degradation (±5%)
5. ✅ Automated enforcement active

### Success Metrics
- **Compliance**: 100% of files under limits
- **Test Pass Rate**: 100% (all tests pass)
- **Build Time**: Not increased by >10%
- **Code Coverage**: Maintained or improved
- **Performance**: Within ±5% of baseline

---

## 📅 Three-Week Timeline

### Week 1: Foundation & Pipeline (Nov 20-26)

**Goal**: Refactor pipeline.cpp and establish enforcement

#### Day 1 (Nov 20): Setup & Infrastructure
- [ ] Install pre-commit hooks on all dev machines
- [ ] Run baseline tests and capture metrics
- [ ] Create `src/core/pipeline_internal.h`
- [ ] Create skeleton files for pipeline modules
- [ ] Update `src/core/CMakeLists.txt`
- [ ] Verify builds with empty files

**Deliverables**:
- Pre-commit hooks active
- 6 new skeleton files created
- Builds successfully

#### Day 2 (Nov 21): Extract Structures & GPU Context
- [ ] Move structures to `pipeline_internal.h` (lines 101-200)
- [ ] Extract GPU context manager to `pipeline_gpu_context.cpp` (lines 851-1050)
- [ ] Build and run tests
- [ ] Fix any compilation errors

**Deliverables**:
- `pipeline_internal.h` complete (~150 lines)
- `pipeline_gpu_context.cpp` complete (~350 lines)
- All tests passing

#### Day 3 (Nov 22): Extract Metrics & Initialization
- [ ] Extract metrics collector to `pipeline_metrics.cpp` (lines 701-790)
- [ ] Extract initialization to `pipeline_initialization.cpp` (lines 201-370, 791-850)
- [ ] Update pipeline.cpp to use new classes
- [ ] Build and run tests

**Deliverables**:
- `pipeline_metrics.cpp` complete (~250 lines)
- `pipeline_initialization.cpp` complete (~300 lines)
- All tests passing

#### Day 4 (Nov 23): Extract Multi-Stream Scheduler (Part 1)
- [ ] Extract scheduler main loop to `pipeline_multistream.cpp` (lines 371-650)
- [ ] Move helper functions (lines 1051-1400)
- [ ] Build and run tests
- [ ] Fix integration issues

**Deliverables**:
- 50% of `pipeline_multistream.cpp` complete
- Partial functionality working

#### Day 5 (Nov 24): Extract Multi-Stream Scheduler (Part 2)
- [ ] Complete `pipeline_multistream.cpp` (lines 1401-2000)
- [ ] Extract batch processor to `pipeline_batch_processing.cpp` (lines 2001-2288)
- [ ] Build and run tests
- [ ] Validate multi-stream still works

**Deliverables**:
- `pipeline_multistream.cpp` complete (~450 lines)
- `pipeline_batch_processing.cpp` complete (~350 lines)
- All tests passing

#### Day 6 (Nov 25): Clean Up & Verify
- [ ] Reduce `pipeline.cpp` to thin wrapper (~300 lines)
- [ ] Run full test suite
- [ ] Performance benchmarks
- [ ] Memory leak checks (valgrind)
- [ ] Update documentation

**Deliverables**:
- `pipeline.cpp` now ~300 lines ✅
- All 6 files under limits ✅
- Tests: 100% pass ✅
- Performance: Within ±5% ✅

#### Day 7 (Nov 26): Week 1 Cleanup & Review
- [ ] Code review of all changes
- [ ] Address review comments
- [ ] Final testing
- [ ] Merge to main branch
- [ ] Tag release: `v0.2.0-refactor-pipeline`

**Milestone**: ✅ Pipeline.cpp refactored - Most critical violation resolved

---

### Week 2: CUDA Kernels (Nov 27 - Dec 3)

**Goal**: Refactor all 3 CUDA kernel files

#### Day 8 (Nov 27): filtering.cu - Setup
- [ ] Create `filtering_device.cuh` with shared helpers (~100 lines)
- [ ] Create skeleton files for filtering modules
- [ ] Update `src/cuda/CMakeLists.txt`
- [ ] Verify builds

**Deliverables**:
- 4 new filtering files created
- Builds successfully

#### Day 9 (Nov 28): filtering.cu - Extraction
- [ ] Extract quality filtering to `filtering_quality.cu` (~150 lines)
- [ ] Extract duplicate marking to `filtering_duplicates.cu` (~150 lines)
- [ ] Build and run CUDA tests
- [ ] Fix any issues

**Deliverables**:
- 2 filtering modules complete
- CUDA tests passing

#### Day 10 (Nov 29): filtering.cu - Completion
- [ ] Extract paired-end validation to `filtering_pairs.cu` (~150 lines)
- [ ] Update main `filtering.cu` to be wrapper (~200 lines)
- [ ] Run full test suite
- [ ] Performance validation

**Deliverables**:
- `filtering.cu` refactored ✅
- 4 files, all under limits ✅
- No performance regression ✅

#### Day 11 (Nov 30): alignment.cu - Refactor
- [ ] Create `alignment_device.cuh` (~100 lines)
- [ ] Extract banded warp kernel to `alignment_banded_warp.cu` (~200 lines)
- [ ] Extract legacy kernel to `alignment_legacy.cu` (~150 lines)
- [ ] Update main `alignment.cu` (~200 lines)
- [ ] Build and test

**Deliverables**:
- `alignment.cu` refactored ✅
- 3 files, all under limits ✅
- Tests passing ✅

#### Day 12 (Dec 1): seeding.cu - Refactor
- [ ] Create `seeding_device.cuh` (~100 lines)
- [ ] Extract optimized kernels to `seeding_optimized.cu` (~200 lines)
- [ ] Extract legacy kernels to `seeding_legacy.cu` (~150 lines)
- [ ] Update main `seeding.cu` (~200 lines)
- [ ] Build and test

**Deliverables**:
- `seeding.cu` refactored ✅
- 3 files, all under limits ✅
- Tests passing ✅

#### Day 13 (Dec 2): Week 2 Testing
- [ ] Run complete test suite (all modules)
- [ ] CUDA memory checks (cuda-memcheck)
- [ ] Performance benchmarks (compare to pre-refactor)
- [ ] Fix any issues found

**Deliverables**:
- All tests: 100% pass ✅
- No memory leaks ✅
- Performance maintained ✅

#### Day 14 (Dec 3): Week 2 Review
- [ ] Code review of all CUDA changes
- [ ] Address review comments
- [ ] Update documentation
- [ ] Merge to main
- [ ] Tag release: `v0.2.1-refactor-cuda`

**Milestone**: ✅ All CUDA files refactored - All critical violations resolved

---

### Week 3: Verification & Enforcement (Dec 4-10)

**Goal**: Full verification and rollout enforcement

#### Day 15 (Dec 4): Integration Testing
- [ ] Build entire project from scratch
- [ ] Run full integration test suite
- [ ] Test multi-stream pipeline end-to-end
- [ ] Test all CUDA kernels with real data
- [ ] Validate output correctness

**Deliverables**:
- Full integration: ✅ PASS
- End-to-end tests: ✅ PASS

#### Day 16 (Dec 5): Performance Validation
- [ ] Run comprehensive benchmarks
- [ ] Compare vs pre-refactor baseline
- [ ] Profile hotspots
- [ ] Validate no regressions
- [ ] Document performance results

**Benchmarks**:
- [ ] Single-threaded alignment (1M reads)
- [ ] Multi-stream pipeline (10M reads)
- [ ] CUDA kernel performance (isolated)
- [ ] Memory usage (peak and average)

**Deliverables**:
- Performance report generated
- All metrics within ±5% of baseline

#### Day 17 (Dec 6): CI/CD Setup
- [ ] Add pre-commit hook to CI pipeline
- [ ] Add file size checks to GitHub Actions
- [ ] Configure automatic blocking on PR
- [ ] Test CI with intentional violation
- [ ] Verify CI correctly rejects oversized files

**Deliverables**:
- CI/CD enforcement active ✅
- Tested and working ✅

#### Day 18 (Dec 7): Documentation Update
- [ ] Update all line number references in docs
- [ ] Update IMPLEMENTATION_PROGRESS.md
- [ ] Add refactoring history to CHANGELOG
- [ ] Create migration guide for developers
- [ ] Update contributor guidelines

**Deliverables**:
- Documentation: 100% updated ✅
- Migration guide complete ✅

#### Day 19 (Dec 8): Team Training
- [ ] Train team on new structure
- [ ] Demo new enforcement hooks
- [ ] Review refactoring patterns
- [ ] Q&A session
- [ ] Distribute documentation

**Deliverables**:
- Team trained ✅
- Guidelines understood ✅

#### Day 20 (Dec 9): Final Verification
- [ ] Full regression test suite
- [ ] Memory profiling
- [ ] Code coverage analysis
- [ ] Performance stress tests
- [ ] Sign-off from tech leads

**Deliverables**:
- All checks pass ✅
- Sign-off received ✅

#### Day 21 (Dec 10): Rollout & Celebrate!
- [ ] Merge to main branch
- [ ] Tag final release: `v0.3.0-refactor-complete`
- [ ] Announce completion
- [ ] Archive old code (if needed)
- [ ] Update project status

**Milestone**: ✅ Refactoring Complete - Zero technical debt!

---

## 📊 Progress Tracking

### Weekly Metrics

**Week 1 Targets**:
- Files refactored: 1 (pipeline.cpp)
- Lines reduced: 2,288 → ~2,000 (across 6 files)
- New files created: 6
- Tests passing: 100%
- Performance: ±5%

**Week 2 Targets**:
- Files refactored: 3 (CUDA files)
- Lines reduced: 1,780 → ~1,400 (across 11 files)
- New files created: 10
- Tests passing: 100%
- Performance: ±5%

**Week 3 Targets**:
- Full project validation
- CI/CD enforcement active
- Documentation complete
- Team trained
- Zero violations

### Daily Standup Questions

1. **What did I complete yesterday?**
2. **What will I complete today?**
3. **Any blockers or issues?**
4. **Tests passing?**
5. **Any performance concerns?**

---

## 🚨 Risk Management

### High-Risk Items

#### Risk 1: Build Breakage
**Probability**: Medium
**Impact**: High
**Mitigation**:
- Extract incrementally (one file at a time)
- Build and test after each extraction
- Keep rollback commits tagged

#### Risk 2: Performance Regression
**Probability**: Low
**Impact**: High
**Mitigation**:
- Benchmark before/after each refactor
- Profile with real data
- Maintain performance test suite
- Accept only if within ±5%

#### Risk 3: Test Failures
**Probability**: Medium
**Impact**: High
**Mitigation**:
- Run tests continuously
- Fix before moving to next file
- Add integration tests before refactoring

#### Risk 4: Timeline Slip
**Probability**: Medium
**Impact**: Medium
**Mitigation**:
- Buffer days built into timeline
- Daily progress tracking
- Escalate blockers immediately

### Contingency Plans

**If Week 1 falls behind:**
- Focus only on pipeline.cpp
- Move CUDA refactoring to Week 3
- Extend timeline by 1 week

**If tests fail repeatedly:**
- Pause refactoring
- Root cause analysis
- Fix tests before continuing

**If performance degrades:**
- Profile and identify cause
- Optimize before moving forward
- Consider alternative extraction strategy

---

## ✅ Acceptance Criteria

### Code Quality
- [ ] All files under 500-line limit
- [ ] Target <300 lines achieved where feasible
- [ ] No functions over 100 lines
- [ ] No complexity over 15
- [ ] Clean module boundaries

### Functionality
- [ ] All existing tests pass
- [ ] No regressions in features
- [ ] Output identical to pre-refactor
- [ ] All edge cases handled

### Performance
- [ ] Within ±5% of baseline
- [ ] No memory leaks
- [ ] Build time not increased >10%
- [ ] No CUDA errors

### Documentation
- [ ] All docs updated
- [ ] Line references corrected
- [ ] Migration guide complete
- [ ] Team trained

### Enforcement
- [ ] Pre-commit hooks active
- [ ] CI/CD blocking violations
- [ ] Monitoring in place
- [ ] Zero new violations possible

---

## 📞 Contacts & Resources

### Documentation
- **Rules**: docs/CODE_ORGANIZATION_RULES.md
- **Setup**: docs/SETUP_CODE_ENFORCEMENT.md
- **Pipeline Plan**: docs/REFACTORING_PLAN_PIPELINE.md
- **CUDA Plan**: docs/REFACTORING_PLAN_CUDA.md
- **Quick Guide**: docs/README_CODE_QUALITY.md

### Tools
- Pre-commit hook: `.githooks/pre-commit`
- Size checker: `find src -name "*.cpp" | xargs wc -l`
- Test suite: `./build/bin/winalign-test`
- Benchmarks: `./build/bin/winalign-benchmark`

### Support
- Questions: Open GitHub issue with `[refactoring]` tag
- Blockers: Escalate to tech lead immediately
- Reviews: Request review in PR description

---

## 📈 Success Celebration

Once complete, we will have:

✅ **Zero files over 500 lines**
✅ **Average file size: <250 lines**
✅ **Clean modular architecture**
✅ **Automated enforcement preventing regression**
✅ **No technical debt**
✅ **Maintainable codebase for future**

**This is a significant achievement that sets WinAlign up for long-term success!**

---

**Status**: 📋 READY TO START
**Next Action**: Begin Day 1 tasks (Setup & Infrastructure)
**Owner**: Development Team
**Last Updated**: 2025-11-19
