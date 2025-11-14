# WinAlign-GPU Architecture

## Overview

WinAlign-GPU is a modular, GPU-accelerated genomic alignment pipeline designed specifically for Windows systems. This document describes the high-level architecture, key components, and design decisions.

## Design Principles

1. **Windows-Native**: Built from the ground up for Windows, not ported from Linux
2. **GPU-First**: Maximize GPU utilization for compute-intensive operations
3. **Memory Efficient**: Stream processing to minimize memory footprint
4. **Modular**: Clean separation between CPU and GPU components
5. **Modern**: Leverage latest CUDA features and C++17 standards

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Input Layer (CPU)                        │
├─────────────────────────────────────────────────────────────┤
│  FASTQ Parser          │          Reference Loader           │
│  - zlib decompression  │          - FM-Index builder         │
│  - Stream processing   │          - GPU memory mapping       │
│  - Quality parsing     │                                     │
└────────────┬───────────┴──────────────┬─────────────────────┘
             │                          │
             v                          v
┌─────────────────────────────────────────────────────────────┐
│              GPU Acceleration Layer (CUDA)                   │
├─────────────────────────────────────────────────────────────┤
│  Stage 1: Seeding                                            │
│  - k-mer extraction (sliding window)                         │
│  - Hash-based seed matching                                  │
│  - CUDA thrust parallel hash lookups                         │
│                                                              │
│  Stage 2: Seed Extension                                     │
│  - Smith-Waterman alignment                                  │
│  - Myers' bit-vector algorithm                               │
│  - Batch processing (1000s of reads)                         │
│  - CIGAR string generation                                   │
│                                                              │
│  Stage 3: Alignment Filtering                                │
│  - Quality score filtering                                   │
│  - Duplicate detection                                       │
│  - Pair validation                                           │
└────────────┬────────────────────────────────────────────────┘
             │
             v
┌─────────────────────────────────────────────────────────────┐
│                    Output Layer (CPU)                        │
├─────────────────────────────────────────────────────────────┤
│  BAM Writer            │          QC Metrics                 │
│  - htslib integration  │          - Alignment statistics     │
│  - Coordinate sorting  │          - Coverage analysis        │
│  - Index generation    │          - Quality scores           │
└─────────────────────────────────────────────────────────────┘
```

## Component Details

### 1. Input Layer (CPU)

#### FASTQ Parser
- **Purpose**: Read and parse paired-end FASTQ/FASTQ.gz files
- **Implementation**: Stream-based processing with zlib
- **Key Features**:
  - On-the-fly decompression
  - Quality score parsing
  - Batch creation for GPU transfer
  - Minimal memory footprint

#### Reference Genome Loader
- **Purpose**: Load and index reference genome
- **Implementation**: FM-Index (Burrows-Wheeler Transform)
- **Key Features**:
  - One-time index building
  - GPU-accessible memory structures
  - Efficient seed lookup
  - Support for large genomes (human: 3GB)

### 2. GPU Acceleration Layer (CUDA)

#### Stage 1: Seeding
- **Algorithm**: Hash-based k-mer matching
- **Parallelism**: One thread per k-mer
- **Data Structure**: Hash table with FM-Index
- **Output**: Candidate alignment positions per read
- **Performance Target**: <1ms for 10K reads

#### Stage 2: Seed Extension
- **Algorithm**: Smith-Waterman with affine gaps
- **Implementation**: Modified GASAL2 kernels
- **Parallelism**: One block per read, warps for DP matrix
- **Output**: Alignment scores, positions, CIGAR strings
- **Performance Target**: <100ms for 10K reads

#### Stage 3: Filtering
- **Operations**:
  - Quality threshold filtering (MAPQ)
  - Coordinate-based duplicate detection
  - Paired-end validation (proper pairs, insert size)
- **Parallelism**: Parallel sort and filter with CUDA thrust
- **Performance Target**: <10ms for 10K reads

### 3. Output Layer (CPU)

#### BAM Writer
- **Library**: htslib (HTSlib)
- **Format**: Compressed BAM with index
- **Features**:
  - Multi-threaded compression
  - Coordinate sorting
  - .bai index generation
  - Header metadata

#### QC Metrics
- **Metrics**:
  - Total reads, aligned reads, % aligned
  - Average quality scores
  - Insert size distribution
  - Coverage statistics
- **Output**: JSON summary file

## Data Flow

### Read Alignment Pipeline

```
1. Input: FASTQ.gz files + Reference FASTA
   ↓
2. CPU: Parse FASTQ, create read batches (1000-10000 reads)
   ↓
3. CPU→GPU: Transfer read batches to GPU memory
   ↓
4. GPU: Seeding (extract k-mers, find candidates)
   ↓
5. GPU: Extension (Smith-Waterman alignment)
   ↓
6. GPU: Filtering (quality, duplicates, pairs)
   ↓
7. GPU→CPU: Transfer aligned reads back
   ↓
8. CPU: Write to BAM file
   ↓
9. Output: BAM + BAI + QC metrics
```

### Memory Management

#### Host (CPU) Memory
- **Read Buffers**: Circular buffer for FASTQ parsing
- **Reference Index**: Shared-memory FM-Index
- **Output Buffer**: Staging buffer for BAM writes

#### Device (GPU) Memory
- **Reference Index**: Read-only, persistent
- **Read Batches**: Reusable device memory pools
- **Alignment Results**: Pre-allocated result arrays

#### Transfer Strategy
- **Pinned Memory**: Host-side buffers for faster transfers
- **Streams**: Multiple CUDA streams for overlap
- **Async Transfers**: Overlap compute and data transfer

## Technology Stack

### Core Technologies
- **Language**: C++17 (host), CUDA 12.8+ (device)
- **Compiler**: MSVC 2022, nvcc
- **Build System**: CMake 3.25+
- **Package Manager**: vcpkg

### Key Libraries
- **zlib**: FASTQ.gz decompression
- **htslib**: BAM/SAM I/O
- **CUDA Toolkit**: GPU computing
- **CUDA Thrust**: Parallel primitives

### GPU Architecture
- **Target**: NVIDIA Ada/Blackwell (RTX 50-series)
- **Compute Capability**: 8.9+
- **Architecture Features**:
  - Tensor cores (optional for ML-based filtering)
  - High memory bandwidth (1.7+ TB/s)
  - Large L2 cache (72+ MB)

## Performance Optimizations

### GPU Optimizations
1. **Coalesced Memory Access**: Align data structures for optimal bandwidth
2. **Shared Memory**: Cache frequently accessed reference regions
3. **Warp-Level Primitives**: Use warp shuffle for reduction operations
4. **Multi-Stream**: Overlap kernel execution and transfers
5. **Persistent Kernels**: Reduce kernel launch overhead

### CPU Optimizations
1. **I/O Buffering**: Large buffers for file I/O
2. **Multi-Threading**: Parallel decompression and BAM writing
3. **SIMD**: Vectorized operations where applicable
4. **Memory Pools**: Reduce allocation overhead

### Algorithmic Optimizations
1. **Adaptive Seeding**: Dynamic k-mer size based on error rate
2. **Early Termination**: Skip extension for low-quality seeds
3. **Hierarchical Filtering**: Multi-stage filtering to reduce data

## Scalability

### Batch Size
- **Small Genome (bacteria)**: 10K reads/batch
- **Large Genome (human)**: 1K-5K reads/batch
- **Trade-off**: Memory usage vs. GPU utilization

### Multi-GPU Support
- **Future Feature**: Distribute work across multiple GPUs
- **Strategy**: Split reference by chromosome
- **Synchronization**: Independent BAM files, merge at end

### Large Reference Genomes
- **Strategy**: Partition reference, stream through GPU
- **Challenge**: FM-Index size for large genomes
- **Solution**: Compressed indices, out-of-core processing

## Error Handling

### GPU Errors
- **CUDA Errors**: Check after kernel launches and transfers
- **Out of Memory**: Graceful degradation to smaller batches
- **Device Lost**: Retry on different GPU or fall back to CPU

### Data Errors
- **Malformed FASTQ**: Skip invalid records, log warnings
- **Quality Issues**: Report in QC metrics
- **Reference Mismatch**: Validate checksums

## Testing Strategy

### Unit Tests
- **CPU Components**: Standard C++ unit tests
- **GPU Kernels**: CUDA unit tests with known inputs
- **Integration**: End-to-end with small test datasets

### Validation
- **Benchmark Datasets**: NA12878, Genome in a Bottle
- **Comparison**: BWA-MEM2, Bowtie2 output
- **Metrics**: Alignment accuracy, CIGAR correctness

### Performance Tests
- **Throughput**: Reads/second on standard hardware
- **Scaling**: Performance vs. batch size, read length
- **Profiling**: NVIDIA Nsight for bottleneck analysis

## Future Extensions

### Phase 1: Core Implementation
- Basic alignment pipeline
- FASTQ → SAM output
- Single-GPU support

### Phase 2: Production Features
- BAM compression and indexing
- Paired-end logic
- Quality filtering
- Duplicate marking

### Phase 3: Advanced Features
- Multi-GPU support
- Variant calling integration
- Machine learning-based filtering
- Real-time alignment (streaming)

### Phase 4: Ecosystem
- Integration with Galaxy, Nextflow
- Cloud deployment (Azure)
- GUI for non-technical users

## References

- **BWA-MEM**: Li H. (2013) Aligning sequence reads, clone sequences and assembly contigs with BWA-MEM. arXiv:1303.3997
- **GASAL2**: Ahmed et al. (2019) GASAL2: a GPU accelerated sequence alignment library for high-throughput NGS data
- **HTSlib**: Bonfield et al. (2021) HTSlib: C library for reading/writing high-throughput sequencing data
- **CUDA Best Practices**: NVIDIA CUDA C++ Programming Guide

---

*Last Updated: 2025-11-14*
