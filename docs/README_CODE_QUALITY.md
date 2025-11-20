# WinAlign Code Quality & Refactoring Guide

**Purpose**: Central hub for all code organization, refactoring, and quality enforcement documentation
**Status**: 🔴 CRITICAL - Action required on oversized files
**Effective Date**: 2025-11-19

---

## 🚨 Quick Start

### For All Developers (5 minutes)

```bash
# 1. Navigate to WinAlign directory
cd /path/to/WinAlign

# 2. Enable code enforcement hooks
git config core.hooksPath .githooks

# 3. Test the hook
.githooks/pre-commit

# You'll see violations for existing files - this is expected
# The hook will prevent NEW violations from being committed
```

**That's it!** You're now protected from committing oversized files.

---

## 📚 Documentation Overview

### 1. CODE_ORGANIZATION_RULES.md - **READ FIRST**
**Purpose**: Mandatory rules for all code
**Key Points**:
- 🚫 Hard limit: 500 lines for source files
- 🚫 Hard limit: 300 lines for headers
- 🚫 Hard limit: 100 lines per function
- ⚠️ Warning at 80% of limits
- ✅ Pre-commit hooks enforce automatically

**When to read**: Before writing any code

### 2. SETUP_CODE_ENFORCEMENT.md - **SETUP GUIDE**
**Purpose**: How to install and configure enforcement tools
**Key Points**:
- Pre-commit hook installation (5 minutes)
- CI/CD integration examples
- Troubleshooting common issues
- Size monitoring tools

**When to read**: First day on project

### 3. REFACTORING_PLAN_PIPELINE.md - **URGENT**
**Purpose**: How to fix pipeline.cpp (2,288 lines → 6 files)
**Key Points**:
- Most critical violation (358% over limit)
- Blocks all other development
- 6-day timeline with daily tasks
- Clear extraction strategy

**When to read**: If you're refactoring pipeline.cpp

### 4. REFACTORING_PLAN_CUDA.md - **HIGH PRIORITY**
**Purpose**: How to fix CUDA kernel files (649, 566, 565 lines)
**Key Points**:
- 3 files need refactoring (filtering, alignment, seeding)
- 7-day timeline
- Detailed extraction plans for each
- Performance validation steps

**When to read**: If you're refactoring CUDA files

---

## 🔴 Current Critical Violations

### Must Fix Immediately (Week 1: Nov 20-26)

```
File: src/core/pipeline.cpp
Lines: 2,288
Limit: 500
Status: ❌ BLOCKING - 358% over limit
Owner: TBD
Deadline: 2025-11-26

Action: See REFACTORING_PLAN_PIPELINE.md
Strategy: Split into 6 modular files
```

### Must Fix Soon (Week 2: Nov 27-Dec 3)

```
File: src/cuda/filtering.cu
Lines: 649
Status: ❌ 30% over limit
Deadline: 2025-12-03

File: src/cuda/alignment.cu
Lines: 566
Status: ❌ 13% over limit
Deadline: 2025-12-03

File: src/cuda/seeding.cu
Lines: 565
Status: ❌ 13% over limit
Deadline: 2025-12-03

Action: See REFACTORING_PLAN_CUDA.md
```

---

## ✅ Compliance Checklist

### Before Starting Work

- [ ] Read CODE_ORGANIZATION_RULES.md
- [ ] Install pre-commit hook (`git config core.hooksPath .githooks`)
- [ ] Test hook works (`.githooks/pre-commit`)
- [ ] Understand your file's current size
- [ ] Know the limits for your file type

### Before Committing Code

- [ ] Run pre-commit hook (automatic if configured)
- [ ] All new/modified files under limits
- [ ] Functions under 100 lines
- [ ] No complexity over 15
- [ ] Extracted common patterns to helpers

### Before Creating PR

- [ ] All files pass pre-commit checks
- [ ] No new violations introduced
- [ ] Tests pass
- [ ] Documentation updated if structure changed
- [ ] Refactoring plan created if approaching limits

---

## 📊 Current Project Status

### File Size Distribution

```
Total source files: 52

Size Category          Count   Percentage
═══════════════════════════════════════════
Under 300 lines        41      79%  ✅
300-400 lines          4       8%   ⚠️
400-500 lines          3       6%   ⚠️
Over 500 lines         4       7%   ❌ CRITICAL

CRITICAL FILES (>500 lines):
  1. pipeline.cpp       2,288 lines  ❌
  2. filtering.cu       649 lines    ❌
  3. alignment.cu       566 lines    ❌
  4. seeding.cu         565 lines    ❌
```

### Refactoring Timeline

```
Week 1 (Nov 20-26):  pipeline.cpp refactoring
Week 2 (Nov 27-Dec 3): CUDA files refactoring
Week 3 (Dec 4-10):   Verification & enforcement rollout
```

---

## 🎯 Goals & Benefits

### Short Term (1-2 weeks)

✅ All files under 500-line hard limit
✅ No files approaching limits (>400 lines)
✅ Pre-commit hooks blocking new violations
✅ CI/CD enforcing on all PRs

### Medium Term (1 month)

✅ Target <300 lines per file achieved
✅ Clear modular structure established
✅ Reduced build times (less coupling)
✅ Easier code review (smaller chunks)

### Long Term (Ongoing)

✅ Maintainable codebase
✅ Easy onboarding for new developers
✅ Low technical debt
✅ Scalable architecture

---

## 🛠️ Tools & Resources

### Checking File Sizes

```bash
# Check all source files
find src -name "*.cpp" -o -name "*.cu" | xargs wc -l | sort -rn

# Check all headers
find include src -name "*.h" -o -name "*.cuh" | xargs wc -l | sort -rn

# Generate detailed report
./check_sizes.sh  # (from SETUP_CODE_ENFORCEMENT.md)
```

### Running Enforcement

```bash
# Test pre-commit hook
.githooks/pre-commit

# Run on all files (not just staged)
git ls-files | grep -E '\.(cpp|cu|c|h|cuh)$' | while read file; do
    [ -f "$file" ] && echo "$(wc -l < "$file") $file"
done | sort -rn
```

### Finding Large Functions

```bash
# Using ctags (if installed)
ctags -x --c++-kinds=f src/core/pipeline.cpp | \
    awk '{print $4, $1}' | \
    sort -rn | \
    head -20

# Look for functions over 100 lines manually
grep -n "^}" src/core/pipeline.cpp | \
    awk -F: '{print $1}' | \
    awk 'NR>1{print ($1-p); p=$1}END{print}'
```

---

## 🚦 Decision Tree: "Should I Refactor?"

```
┌─────────────────────────────────┐
│ Is my file over 500 lines?     │
└────────┬────────────────────────┘
         │
    YES  │  NO
    ─────┴──────
    │          │
    ▼          ▼
┌────────┐  ┌──────────────────────────┐
│ MUST   │  │ Is my file over 400?     │
│ REFACTOR│  └────────┬─────────────────┘
│ NOW!   │           │
└────────┘      YES  │  NO
                ─────┴──────
                │          │
                ▼          ▼
        ┌──────────┐  ┌────────────────┐
        │ SHOULD   │  │ Is my file over│
        │ REFACTOR │  │ 300 lines?     │
        │ SOON     │  └────────┬───────┘
        └──────────┘           │
                          YES  │  NO
                          ─────┴──────
                          │          │
                          ▼          ▼
                  ┌──────────┐  ┌────────┐
                  │ CONSIDER │  │ OK FOR │
                  │ REFACTOR │  │ NOW    │
                  └──────────┘  └────────┘
```

---

## 📞 Getting Help

### Questions About Rules?

- Read: CODE_ORGANIZATION_RULES.md
- Check: Examples in refactoring plans
- Ask: Team lead or senior developer

### Need Refactoring Help?

- Template: REFACTORING_PLAN_PIPELINE.md (comprehensive example)
- CUDA-specific: REFACTORING_PLAN_CUDA.md
- Ask: Code review before major refactoring

### Hook Not Working?

- Troubleshooting: SETUP_CODE_ENFORCEMENT.md
- Test: `.githooks/pre-commit` manually
- Verify: `git config core.hooksPath` shows `.githooks`

---

## 📈 Success Metrics

Track these metrics weekly:

1. **Violation Count**: # of files over 500 lines (Target: 0)
2. **Warning Count**: # of files over 400 lines (Target: <5)
3. **Average File Size**: Mean lines per file (Target: <250)
4. **Largest File**: Max file size (Target: <400)
5. **Compliance Rate**: % files under 300 lines (Target: >90%)

---

## 🎓 Learning Resources

### Before Refactoring

- **Read**: "Clean Code" by Robert C. Martin (Ch. 3, 10)
- **Read**: CODE_ORGANIZATION_RULES.md (Anti-patterns section)
- **Study**: Existing refactoring plans as templates

### During Refactoring

- **Follow**: REFACTORING_PLAN_*.md step-by-step
- **Test**: After each extraction step
- **Verify**: No performance regressions

### After Refactoring

- **Document**: Update references in IMPLEMENTATION_PROGRESS.md
- **Review**: Get code review before merging
- **Monitor**: Watch CI for any issues

---

## 🔄 Maintenance

### Weekly Tasks

- [ ] Run size report (`./check_sizes.sh`)
- [ ] Review new commits for compliance
- [ ] Update refactoring progress
- [ ] Address any new violations

### Monthly Tasks

- [ ] Review and update limits if needed
- [ ] Refine refactoring templates
- [ ] Train new team members
- [ ] Celebrate reduced technical debt!

---

## 📝 Quick Reference

| What | Where | When |
|------|-------|------|
| Hard limits | CODE_ORGANIZATION_RULES.md | Always |
| Setup hooks | SETUP_CODE_ENFORCEMENT.md | First day |
| Fix pipeline.cpp | REFACTORING_PLAN_PIPELINE.md | Week 1 |
| Fix CUDA files | REFACTORING_PLAN_CUDA.md | Week 2 |
| Size check | `.githooks/pre-commit` | Every commit |
| CI check | GitHub Actions / GitLab CI | Every PR |

---

**Last Updated**: 2025-11-19
**Next Review**: 2025-12-01
**Status**: 🔴 ACTIVE ENFORCEMENT

**Questions?** See individual documentation files or ask team lead.
