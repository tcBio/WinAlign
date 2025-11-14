# Building WinAlign-GPU

This guide covers building WinAlign-GPU from source on Windows.

## Prerequisites

### Required Software

1. **Windows 10/11 (64-bit)**
   - Windows 10 version 1809 or later
   - Windows 11 recommended

2. **Visual Studio 2022**
   - Download from: https://visualstudio.microsoft.com/
   - Install "Desktop development with C++" workload
   - Ensure C++17 support is included

3. **CUDA Toolkit 12.8+**
   - Download from: https://developer.nvidia.com/cuda-downloads
   - Install with default settings
   - Ensure CUDA is added to PATH

4. **CMake 3.25+**
   - Download from: https://cmake.org/download/
   - Add to PATH during installation

5. **vcpkg (Package Manager)**
   - Clone vcpkg repository:
     ```cmd
     git clone https://github.com/microsoft/vcpkg.git
     cd vcpkg
     .\bootstrap-vcpkg.bat
     ```
   - Set environment variable (optional):
     ```cmd
     setx VCPKG_ROOT "C:\path\to\vcpkg"
     ```

### Hardware Requirements

- **GPU**: NVIDIA GPU with Compute Capability 8.9+ (RTX 50-series recommended)
- **RAM**: 16GB minimum, 32GB recommended
- **Storage**: 10GB for build artifacts and dependencies

## Installing Dependencies

### Using vcpkg

```cmd
cd %VCPKG_ROOT%

# Install zlib
vcpkg install zlib:x64-windows

# Install htslib (if available)
vcpkg install htslib:x64-windows

# Integrate with Visual Studio (optional)
vcpkg integrate install
```

### Manual Installation (if needed)

If vcpkg doesn't have a package, you can build dependencies manually:

#### zlib
```cmd
git clone https://github.com/madler/zlib.git
cd zlib
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=C:/local/zlib
cmake --build . --config Release --target install
```

#### htslib
```cmd
git clone https://github.com/samtools/htslib.git
cd htslib
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=C:/local/htslib
cmake --build . --config Release --target install
```

## Building WinAlign-GPU

### Step 1: Clone the Repository

```cmd
git clone https://github.com/tcBio/WinAlign.git
cd WinAlign
```

### Step 2: Configure with CMake

#### Using vcpkg (Recommended)

```cmd
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
         -DCMAKE_BUILD_TYPE=Release ^
         -DCMAKE_CUDA_ARCHITECTURES=89
```

#### Without vcpkg

```cmd
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release ^
         -DCMAKE_CUDA_ARCHITECTURES=89 ^
         -DZLIB_ROOT=C:/local/zlib ^
         -DHTSLIB_ROOT=C:/local/htslib
```

### Step 3: Build

```cmd
# Build Release version
cmake --build . --config Release

# Or build with MSBuild
msbuild WinAlign.sln /p:Configuration=Release
```

### Step 4: Run Tests (Optional)

```cmd
ctest -C Release --output-on-failure
```

### Step 5: Install (Optional)

```cmd
cmake --install . --prefix C:/Program Files/WinAlign
```

## Build Options

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_TESTS` | `ON` | Build unit tests |
| `BUILD_BENCHMARKS` | `ON` | Build benchmark suite |
| `ENABLE_PROFILING` | `OFF` | Enable CUDA profiling |
| `CMAKE_CUDA_ARCHITECTURES` | `89` | Target GPU architecture |

### Examples

```cmd
# Build without tests
cmake .. -DBUILD_TESTS=OFF

# Build with profiling enabled
cmake .. -DENABLE_PROFILING=ON

# Target multiple GPU architectures
cmake .. -DCMAKE_CUDA_ARCHITECTURES="75;80;89"
```

## GPU Architecture Selection

Set `CMAKE_CUDA_ARCHITECTURES` based on your GPU:

| GPU Series | Architecture | Compute Capability |
|------------|--------------|-------------------|
| RTX 50-series | Blackwell | 89 |
| RTX 40-series | Ada Lovelace | 89 |
| RTX 30-series | Ampere | 86 |
| RTX 20-series | Turing | 75 |

## Troubleshooting

### CUDA Not Found

```
CMake Error: CUDA not found
```

**Solution**: Ensure CUDA Toolkit is installed and added to PATH:
```cmd
set PATH=%PATH%;C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin
```

### vcpkg Package Not Found

```
error: package 'htslib' not found
```

**Solution**: Check available packages:
```cmd
vcpkg search htslib
```

If not available, build manually or use alternative packages.

### Visual Studio Version Mismatch

```
error MSB8020: The build tools for v143 cannot be found
```

**Solution**: Install Visual Studio 2022 or update CMake to use installed version:
```cmd
cmake .. -G "Visual Studio 17 2022"
```

### Out of Memory During Build

**Solution**: Reduce parallel build jobs:
```cmd
cmake --build . --config Release -- /m:2
```

### CUDA Architecture Mismatch

```
error: unsupported GPU architecture 'compute_89'
```

**Solution**: Update CUDA Toolkit or use supported architecture:
```cmd
cmake .. -DCMAKE_CUDA_ARCHITECTURES=86
```

## Building for Development

### Debug Build

```cmd
mkdir build-debug
cd build-debug
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_PROFILING=ON
cmake --build . --config Debug
```

### With NVIDIA Nsight Integration

```cmd
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build . --config RelWithDebInfo
```

Then open the project in NVIDIA Nsight Compute or Nsight Systems.

## Cross-Compilation

WinAlign-GPU is designed for Windows only. Cross-compilation from Linux is not supported.

## Continuous Integration

For CI/CD builds, see `.github/workflows/build.yml` (if available).

## Next Steps

After building:

1. Run the executable: `.\bin\Release\winalign-gpu.exe --help`
2. Run tests: `ctest -C Release`
3. Read [USAGE.md](USAGE.md) for usage instructions

## Support

If you encounter build issues:

1. Check [GitHub Issues](https://github.com/tcBio/WinAlign/issues)
2. Open a new issue with:
   - CMake output
   - Build errors
   - System information

---

*Last Updated: 2025-11-14*
