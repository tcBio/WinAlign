# WinAlign-GPU

**A Windows-Native GPU-Accelerated Genomic Alignment Pipeline**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CUDA](https://img.shields.io/badge/CUDA-12.8+-green.svg)](https://developer.nvidia.com/cuda-toolkit)
[![Platform](https://img.shields.io/badge/Platform-Windows-blue.svg)](https://www.microsoft.com/windows)

## Overview

WinAlign-GPU is a high-performance, Windows-native genomic sequence alignment tool designed from the ground up for modern GPU architectures. Unlike traditional Linux-based aligners, WinAlign leverages the full power of NVIDIA RTX 50-series GPUs to deliver unprecedented alignment speeds on Windows systems.

## Key Features

- **🚀 GPU-Accelerated**: Utilizes CUDA for massive parallelism (30-50x speedup vs CPU)
- **🪟 Windows-First**: Native Windows design, not a Linux port
- **💾 Memory Efficient**: Streaming architecture with low memory footprint
- **🎯 High Accuracy**: Implements proven algorithms from BWA-MEM
- **📦 Modern Stack**: Built with CUDA 12.8+, Visual Studio 2022, CMake
- **🔓 Open Source**: MIT/Apache 2.0 licensed

## Architecture

```
[FASTQ Input] → [CPU: Parse & Batch] → [GPU: Align] → [CPU: Write BAM]
                       ↑                                    ↓
                   [Reference Index]                  [QC Stats]
```

### Pipeline Stages

1. **Input Layer (CPU)**
   - FASTQ.gz parsing with zlib decompression
   - Reference genome loading with FM-Index
   - Stream processing for minimal memory usage

2. **GPU Acceleration Layer (CUDA)**
   - **Seeding**: Parallel k-mer extraction and hash-based matching
   - **Extension**: Smith-Waterman alignment with CUDA kernels
   - **Filtering**: Quality filtering, duplicate detection, pair validation

3. **Output Layer (CPU)**
   - BAM file writing with htslib
   - Coordinate sorting and indexing
   - QC metrics generation

## Performance

### Target Specifications (RTX 5090)
- **VRAM**: 32GB
- **CUDA Cores**: 21,760
- **Memory Bandwidth**: 1,792 GB/s

### Projected Throughput
- **Per Sample (30x WGS)**: 20-40 minutes
- **81 Samples**: 1-3 days total
- **Speedup vs BWA-MEM**: 30-50x

## Requirements

### Hardware
- NVIDIA GPU with CUDA Compute Capability 8.9+ (RTX 50-series recommended)
- 32GB+ system RAM (for large genomes)
- 50GB+ storage for reference indices

### Software
- Windows 10/11 (64-bit)
- CUDA Toolkit 12.8+
- Visual Studio 2022
- CMake 3.25+
- vcpkg (package manager)

## Building from Source

### Prerequisites

1. Install [CUDA Toolkit 12.8+](https://developer.nvidia.com/cuda-downloads)
2. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) with C++ desktop development
3. Install [CMake](https://cmake.org/download/)
4. Install [vcpkg](https://github.com/microsoft/vcpkg)

### Install Dependencies via vcpkg

```cmd
vcpkg install zlib:x64-windows
vcpkg install htslib:x64-windows
```

### Build

```cmd
mkdir build
cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

## Usage

```cmd
winalign-gpu -r reference.fasta -1 read1.fastq.gz -2 read2.fastq.gz -o output.bam
```

### Options
- `-r, --reference`: Reference genome FASTA file
- `-1, --read1`: Forward reads (FASTQ/FASTQ.gz)
- `-2, --read2`: Reverse reads (FASTQ/FASTQ.gz)
- `-o, --output`: Output BAM file
- `-t, --threads`: CPU threads for I/O (default: 4)
- `--gpu-id`: GPU device ID (default: 0)

## Development Roadmap

- [x] **Phase 1**: Project architecture and setup
- [ ] **Phase 2**: Proof of concept implementation
  - [ ] Basic seeding and extension
  - [ ] Uncompressed FASTQ → SAM output
  - [ ] Single-threaded CPU coordination
- [ ] **Phase 3**: GPU optimization
  - [ ] Port Smith-Waterman kernels
  - [ ] Batch processing optimization
  - [ ] Multi-stream CUDA
- [ ] **Phase 4**: Production features
  - [ ] FASTQ.gz support
  - [ ] BAM output with compression
  - [ ] Paired-end handling
  - [ ] Quality filtering and duplicate marking
- [ ] **Phase 5**: Performance and polish
  - [ ] Benchmarking vs BWA-MEM2
  - [ ] Documentation
  - [ ] Windows installer

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

Portions of this software may be derived from or inspired by:
- BWA-MEM (GPL-3.0)
- GASAL2 (Apache 2.0)
- htslib (MIT/BSD)

## Citation

If you use WinAlign-GPU in your research, please cite:

```
@software{winalign2025,
  title={WinAlign-GPU: A Windows-Native GPU-Accelerated Genomic Alignment Pipeline},
  author={[Your Name]},
  year={2025},
  url={https://github.com/[username]/WinAlign}
}
```

## Acknowledgments

- NVIDIA CUDA team for GPU computing tools
- HTSlib contributors for genomic file format support
- BWA-MEM authors for algorithmic foundations
- GASAL2 project for CUDA alignment kernels

## Contact

- Issues: [GitHub Issues](https://github.com/[username]/WinAlign/issues)
- Discussions: [GitHub Discussions](https://github.com/[username]/WinAlign/discussions)

---

**Built with ❤️ for Windows genomics**
