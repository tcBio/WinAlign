# Code Enforcement Setup Guide

**Purpose**: Install and configure automated enforcement of code organization rules
**See Also**: CODE_ORGANIZATION_RULES.md

---

## 🚀 Quick Setup (5 minutes)

### Step 1: Install Git Hooks

```bash
# Navigate to WinAlign directory
cd /path/to/WinAlign

# Configure git to use custom hooks directory
git config core.hooksPath .githooks

# Verify hook is executable
ls -la .githooks/pre-commit

# Should show: -rwxr-xr-x (executable)
```

### Step 2: Test the Hook

```bash
# Test on current files (will show violations)
.githooks/pre-commit

# Expected output:
# ❌ BLOCKED: src/core/pipeline.cpp
#    Lines: 2288 (limit: 500 for source files)
#    Over by: 1788 lines (358% over)
```

### Step 3: Verify Installation

```bash
# Try to commit a large file
echo "int main() {}" > test_large.cpp
for i in {1..501}; do echo "// Line $i" >> test_large.cpp; done
git add test_large.cpp
git commit -m "Test commit"

# Should be BLOCKED with error message
# Clean up
git reset HEAD test_large.cpp
rm test_large.cpp
```

---

## 📝 Hook Behavior

### Limits Enforced

| File Type | Hard Limit | Warning at |
|-----------|-----------|------------|
| Source (.cpp, .cu, .c) | 500 lines | 400 lines (80%) |
| Headers (.h, .cuh, .hpp) | 300 lines | 240 lines (80%) |
| Tests (*_test.cpp) | 400 lines | 320 lines (80%) |

### Example Outputs

**1. All files pass:**
```bash
$ git commit -m "Small change"

🔍 Checking code organization rules...

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ All files within size limits

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[main abc1234] Small change
 1 file changed, 10 insertions(+)
```

**2. Warning (file approaching limit):**
```bash
$ git commit -m "Add feature"

🔍 Checking code organization rules...

⚠️  WARNING: src/utils/helper.cpp
   Lines: 420 (limit: 500 for source files)
   Approaching limit: 84% used

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
⚠️  WARNINGS DETECTED

  1 file(s) approaching size limits (>80%)
  Consider refactoring soon to stay under limits

  See docs/CODE_ORGANIZATION_RULES.md for guidance

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
✅ Proceeding with commit

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
[main abc1234] Add feature
 1 file changed, 50 insertions(+)
```

**3. Blocked (file over limit):**
```bash
$ git commit -m "Add large file"

🔍 Checking code organization rules...

❌ BLOCKED: src/core/new_module.cpp
   Lines: 650 (limit: 500 for source files)
   Over by: 150 lines (30% over)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
❌ COMMIT BLOCKED

  1 file(s) exceed size limits

  Please refactor before committing:
  • Source files: max 500 lines
  • Header files: max 300 lines
  • Test files: max 400 lines

  See docs/CODE_ORGANIZATION_RULES.md for guidance
  See docs/REFACTORING_PLAN_PIPELINE.md for examples

  To bypass this check (NOT RECOMMENDED):
  git commit --no-verify -m 'your message'

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
```

---

## 🔧 Configuration

### Adjusting Limits

Edit `.githooks/pre-commit`:

```bash
# Configuration section (lines 6-8)
MAX_SOURCE_LINES=500  # Change this
MAX_HEADER_LINES=300  # Change this
MAX_TEST_LINES=400    # Change this
```

**WARNING**: Only adjust limits after team discussion. Current limits are based on industry best practices.

### Disabling Hook Temporarily

```bash
# Option 1: Bypass single commit (NOT RECOMMENDED)
git commit --no-verify -m "Emergency fix"

# Option 2: Disable hooks directory
git config --unset core.hooksPath

# Re-enable later
git config core.hooksPath .githooks
```

---

## 🤖 CI/CD Integration

### GitHub Actions

Add to `.github/workflows/code-quality.yml`:

```yaml
name: Code Quality Checks

on:
  push:
    branches: [ main, develop ]
  pull_request:
    branches: [ main, develop ]

jobs:
  check-file-sizes:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Check file size limits
        run: |
          chmod +x .githooks/pre-commit
          .githooks/pre-commit

      - name: Report violations
        if: failure()
        run: |
          echo "❌ Code organization rules violated"
          echo "See docs/CODE_ORGANIZATION_RULES.md"
          exit 1
```

### GitLab CI

Add to `.gitlab-ci.yml`:

```yaml
code-quality:
  stage: test
  script:
    - chmod +x .githooks/pre-commit
    - .githooks/pre-commit
  rules:
    - if: '$CI_PIPELINE_SOURCE == "merge_request_event"'
    - if: '$CI_COMMIT_BRANCH == "main"'
```

---

## 📊 Monitoring and Reporting

### Check All Files

```bash
# Run hook on all tracked files (not just staged)
git ls-files | grep -E '\.(cpp|cu|c|h|cuh|hpp)$' | while read file; do
    if [ -f "$file" ]; then
        LINES=$(wc -l < "$file")
        if [ $LINES -gt 500 ]; then
            echo "❌ $file: $LINES lines"
        elif [ $LINES -gt 400 ]; then
            echo "⚠️  $file: $LINES lines"
        fi
    fi
done
```

### Generate Size Report

```bash
# Create detailed size report
cat > check_sizes.sh << 'EOF'
#!/bin/bash

echo "WinAlign File Size Report"
echo "Generated: $(date)"
echo ""
echo "Source Files (limit: 500 lines)"
echo "═══════════════════════════════════════════════"

find src -type f \( -name "*.cpp" -o -name "*.cu" -o -name "*.c" \) | while read file; do
    LINES=$(wc -l < "$file")
    PCT=$((100 * LINES / 500))
    if [ $LINES -gt 500 ]; then
        STATUS="❌ OVER"
    elif [ $LINES -gt 400 ]; then
        STATUS="⚠️  WARN"
    else
        STATUS="✅ OK  "
    fi
    printf "%s %4d lines (%3d%%) %s\n" "$STATUS" "$LINES" "$PCT" "$file"
done | sort -rn -k2

echo ""
echo "Header Files (limit: 300 lines)"
echo "═══════════════════════════════════════════════"

find include src -type f \( -name "*.h" -o -name "*.cuh" -o -name "*.hpp" \) | while read file; do
    LINES=$(wc -l < "$file")
    PCT=$((100 * LINES / 300))
    if [ $LINES -gt 300 ]; then
        STATUS="❌ OVER"
    elif [ $LINES -gt 240 ]; then
        STATUS="⚠️  WARN"
    else
        STATUS="✅ OK  "
    fi
    printf "%s %4d lines (%3d%%) %s\n" "$STATUS" "$LINES" "$PCT" "$file"
done | sort -rn -k2
EOF

chmod +x check_sizes.sh
./check_sizes.sh
```

---

## 🚨 Troubleshooting

### Problem: Hook not running

**Symptoms**: Can commit large files without any checks

**Solution**:
```bash
# Check git hooks configuration
git config core.hooksPath

# Should output: .githooks

# If empty, set it
git config core.hooksPath .githooks

# Verify hook is executable
ls -la .githooks/pre-commit

# If not executable
chmod +x .githooks/pre-commit
```

### Problem: Hook running but not blocking

**Symptoms**: See warnings but commit succeeds

**Solution**:
```bash
# Check hook exit codes
bash -x .githooks/pre-commit

# Hook should exit with code 1 if violations found
# Check for scripting errors in output
```

### Problem: Hook blocks valid files

**Symptoms**: Files under limit are still blocked

**Solution**:
```bash
# Verify line count manually
wc -l src/your/file.cpp

# Check for non-ASCII characters or encoding issues
file src/your/file.cpp

# Hook only counts actual file lines, so should match wc -l
```

### Problem: Want to commit emergency fix

**Temporary bypass** (use sparingly):
```bash
# Bypass hook for single commit
git commit --no-verify -m "Emergency: Critical bug fix"

# IMPORTANT: File a refactoring task immediately
# Create issue: "Refactor <file> to meet size limits"
```

---

## 📚 Additional Resources

- **CODE_ORGANIZATION_RULES.md** - Complete rule definitions
- **REFACTORING_PLAN_PIPELINE.md** - Example refactoring (pipeline.cpp)
- **REFACTORING_PLAN_CUDA.md** - CUDA kernel refactoring examples

---

## ✅ Post-Installation Checklist

- [ ] Git hooks configured (`git config core.hooksPath` shows `.githooks`)
- [ ] Hook is executable (`ls -la .githooks/pre-commit` shows `x` permission)
- [ ] Hook runs on commit attempt (test with dummy file)
- [ ] Hook correctly blocks oversized files (test with 501-line file)
- [ ] Hook shows warnings for files approaching limit (test with 450-line file)
- [ ] Team members all have hooks configured
- [ ] CI/CD pipeline includes size checks
- [ ] Size report generated and reviewed

---

**Installation Time**: 5 minutes
**Impact**: Prevents code quality degradation from day one
**Maintenance**: Zero (automated)

**This enforcement is MANDATORY for all developers starting 2025-11-20**
