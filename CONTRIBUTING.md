# Contributing to WinAlign-GPU

Thank you for your interest in contributing to WinAlign-GPU! This document provides guidelines for contributing to the project.

## Code of Conduct

We are committed to providing a welcoming and inclusive environment. Please be respectful and professional in all interactions.

## Getting Started

1. **Fork the repository** on GitHub
2. **Clone your fork** locally:
   ```cmd
   git clone https://github.com/YOUR_USERNAME/WinAlign.git
   cd WinAlign
   ```
3. **Create a branch** for your feature or bugfix:
   ```cmd
   git checkout -b feature/my-new-feature
   ```

## Development Environment

### Prerequisites
- Windows 10/11 (64-bit)
- Visual Studio 2022 with C++ desktop development
- CUDA Toolkit 12.8 or later
- CMake 3.25 or later
- vcpkg for package management

### Building

```cmd
# Install dependencies
vcpkg install zlib:x64-windows htslib:x64-windows

# Configure
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build . --config Release
```

## Coding Standards

### C++ Style
- Follow **C++17** standards
- Use **4 spaces** for indentation (no tabs)
- Use **snake_case** for variables and functions
- Use **PascalCase** for class names
- Use **UPPER_CASE** for constants and macros

### CUDA Style
- Use `.cu` extension for CUDA source files
- Use `.cuh` extension for CUDA headers
- Keep kernel code separate from host code
- Document kernel launch parameters

### Example

```cpp
// Good
class AlignmentEngine {
public:
    void process_batch(const ReadBatch& batch);

private:
    size_t batch_size_;
};

// Bad
class alignmentEngine {
public:
    void ProcessBatch(const ReadBatch& Batch);

private:
    size_t batchSize;
};
```

## Testing

- Add tests for all new features
- Ensure existing tests pass before submitting PR
- Run tests with: `ctest --output-on-failure`

## Pull Request Process

1. **Update documentation** if adding new features
2. **Add tests** for your changes
3. **Update README.md** if necessary
4. **Ensure all tests pass**
5. **Create a pull request** with a clear description:
   - What problem does it solve?
   - How does it work?
   - Any breaking changes?

### PR Template

```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Performance improvement
- [ ] Documentation update

## Testing
How have you tested these changes?

## Checklist
- [ ] Code follows project style guidelines
- [ ] Tests added/updated
- [ ] Documentation updated
- [ ] All tests pass
```

## Commit Messages

Use clear, descriptive commit messages:

```
feat: Add Smith-Waterman GPU kernel
fix: Correct memory leak in FASTQ parser
docs: Update architecture documentation
perf: Optimize seed filtering on GPU
```

### Prefixes
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation
- `style`: Code style changes
- `refactor`: Code refactoring
- `perf`: Performance improvement
- `test`: Test updates
- `chore`: Build/tooling changes

## Reporting Issues

When reporting bugs, please include:

1. **System information**: Windows version, GPU model, CUDA version
2. **Steps to reproduce**: Minimal example to reproduce the issue
3. **Expected behavior**: What should happen
4. **Actual behavior**: What actually happens
5. **Error messages**: Full error output if applicable

## Feature Requests

We welcome feature requests! Please:

1. **Check existing issues** to avoid duplicates
2. **Describe the use case**: Why is this feature needed?
3. **Propose a solution**: How should it work?
4. **Consider alternatives**: What other approaches could work?

## Performance Contributions

For performance improvements:

1. **Provide benchmarks**: Before/after measurements
2. **Document methodology**: How did you measure?
3. **Test on multiple GPUs**: If possible
4. **Profile the code**: Use NVIDIA Nsight Compute

## Documentation

- Update relevant `.md` files in `docs/`
- Add inline comments for complex algorithms
- Document public APIs with Doxygen-style comments
- Update README.md for user-facing changes

## License

By contributing, you agree that your contributions will be licensed under the MIT License.

## Questions?

Feel free to open an issue or discussion if you have questions about contributing!

---

Thank you for contributing to WinAlign-GPU! 🚀
