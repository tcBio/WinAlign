# WinAlign-Amplicon: Design Document

**Version**: 0.1.0
**Status**: Design Phase
**Target**: Phylos-style amplicon sequencing data

---

## Overview

WinAlign-Amplicon is a specialized variant of the WinAlign pipeline optimized for amplicon sequencing data (e.g., Phylos cannabis genotyping panels). Unlike WGS data, amplicon data has unique characteristics that require a different processing strategy.

**This tool is SEPARATE from the main WinAlign-GPU WGS pipeline.**

---

## Key Differences: WGS vs Amplicon

| Feature | WGS (WinAlign-GPU) | Amplicon (WinAlign-Amplicon) |
|---------|-------------------|------------------------------|
| Coverage | 30x uniform | 10,000-100,000x at target loci |
| Genome Coverage | Entire genome | ~100 loci (0.01% of genome) |
| Read Diversity | High (unique positions) | Low (same loci repeated) |
| GPU Utilization | Excellent (diverse alignments) | Poor (redundant alignments) |
| Primary Challenge | Throughput | Deduplication, read collapse |
| Key Optimization | Parallelism | Pre-processing efficiency |

---

## Architecture

### Design Philosophy

**Core Principle**: Collapse identical reads BEFORE GPU processing

```
Traditional Amplicon Pipeline (INEFFICIENT):
10,000 reads → 10,000 GPU alignments → 9,999 duplicates filtered

WinAlign-Amplicon (EFFICIENT):
10,000 reads → Collapse to 50 unique → 50 GPU alignments → Track counts
```

### Pipeline Stages

```
┌─────────────────────────────────────────────────────────────┐
│ Stage 1: Read Ingestion (CPU)                               │
│ - FASTQ parsing with quality filtering                      │
│ - Per-amplicon read assignment                              │
└────────────────────┬────────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────────┐
│ Stage 2: Read Collapsing (CPU/GPU Hybrid)                   │
│ - Hash-based deduplication                                  │
│ - Cluster near-identical reads (1-2bp differences)          │
│ - Track read counts per unique sequence                     │
│ - Reduces 10,000 → ~50-500 unique sequences                 │
└────────────────────┬────────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────────┐
│ Stage 3: Amplicon-Aware Alignment (GPU)                     │
│ - Direct mapping to known amplicon positions                │
│ - No k-mer seeding (positions are known!)                   │
│ - Smith-Waterman only for ambiguous cases                   │
│ - Batch unique sequences, not raw reads                     │
└────────────────────┬────────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────────┐
│ Stage 4: Variant Calling (GPU-Accelerated)                  │
│ - Pileup generation at target positions                     │
│ - SNP calling with depth-aware quality                      │
│ - Indel detection within amplicons                          │
│ - Generate VCF with read depth annotations                  │
└────────────────────┬────────────────────────────────────────┘
                     ↓
┌─────────────────────────────────────────────────────────────┐
│ Stage 5: Output & QC (CPU)                                  │
│ - VCF writing (not BAM - amplicon-specific format)          │
│ - Per-amplicon coverage metrics                             │
│ - Quality control report                                    │
│ - Genotype concordance checks                               │
└─────────────────────────────────────────────────────────────┘
```

---

## Data Structures

### Amplicon Target Definition

```cpp
struct AmpliconTarget {
    std::string amplicon_id;        // e.g., "PHYLOS_SNP_0001"
    std::string chromosome;         // e.g., "chr1"
    uint64_t start;                 // 0-based start position
    uint64_t end;                   // 0-based end position
    std::string primer_fwd;         // Forward primer sequence
    std::string primer_rev;         // Reverse primer sequence
    uint32_t expected_length;       // Expected amplicon length
    std::vector<uint64_t> snp_positions;  // Known SNP positions within amplicon
};
```

### Read Cluster (Collapsed Reads)

```cpp
struct ReadCluster {
    std::string consensus_sequence;  // Representative sequence
    std::string consensus_quality;   // Merged quality scores
    uint32_t read_count;             // Number of collapsed reads
    std::vector<ReadID> read_ids;    // Original read identifiers
    float avg_quality;               // Average quality score
    std::string amplicon_id;         // Assigned amplicon
};
```

### Amplicon Alignment Result

```cpp
struct AmpliconAlignment {
    std::string amplicon_id;         // Which amplicon
    uint64_t position;               // Alignment position
    std::string cigar;               // CIGAR string
    uint32_t read_count;             // Collapsed read count
    uint8_t mapping_quality;         // MAPQ
    bool is_expected_position;       // Aligns to expected target?
};
```

### Variant Call (Amplicon-Specific)

```cpp
struct AmpliconVariant {
    std::string amplicon_id;         // Source amplicon
    uint64_t position;               // Genomic position
    char ref_allele;                 // Reference base
    char alt_allele;                 // Alternate base
    uint32_t ref_depth;              // Reads supporting ref
    uint32_t alt_depth;              // Reads supporting alt
    float allele_frequency;          // alt_depth / total_depth
    uint8_t quality;                 // Variant quality score
    bool is_known_marker;            // Expected variant position?
};
```

---

## Key Algorithms

### 1. Read Collapsing Algorithm

**CPU-based hashing** (Stage 2):

```cpp
std::unordered_map<uint64_t, ReadCluster> collapse_reads(
    const std::vector<Read>& reads,
    uint32_t max_distance = 2  // Allow 1-2bp differences
) {
    std::unordered_map<uint64_t, ReadCluster> clusters;

    for (const auto& read : reads) {
        // Hash sequence
        uint64_t hash = xxhash64(read.sequence);

        // Find existing cluster or create new
        if (clusters.count(hash)) {
            clusters[hash].read_count++;
            clusters[hash].read_ids.push_back(read.id);
            // Update consensus quality (higher quality bases win)
            merge_quality_scores(clusters[hash], read);
        } else {
            // New cluster
            ReadCluster cluster;
            cluster.consensus_sequence = read.sequence;
            cluster.consensus_quality = read.quality;
            cluster.read_count = 1;
            cluster.read_ids.push_back(read.id);
            clusters[hash] = cluster;
        }
    }

    // Optional: Merge near-identical clusters
    merge_similar_clusters(clusters, max_distance);

    return clusters;
}
```

### 2. Amplicon Assignment (No K-mer Seeding!)

**Direct position lookup** instead of k-mer matching:

```cpp
std::string assign_amplicon(
    const std::string& read_sequence,
    const std::vector<AmpliconTarget>& targets
) {
    // Check primer presence (faster than alignment)
    for (const auto& target : targets) {
        // Look for forward or reverse primer in read
        if (contains_primer(read_sequence, target.primer_fwd) ||
            contains_primer(read_sequence, target.primer_rev)) {
            return target.amplicon_id;
        }
    }

    // Fallback: simple alignment to target regions
    // (much faster than genome-wide search)
    return assign_by_alignment(read_sequence, targets);
}
```

### 3. GPU Pileup Generation

**Parallel pileup at known SNP positions**:

```cuda
__global__ void generate_pileup_kernel(
    const AmpliconAlignment* alignments,
    uint32_t num_alignments,
    const uint64_t* snp_positions,
    uint32_t num_snps,
    const char* reference,
    const char* reads,
    uint32_t* ref_counts,
    uint32_t* alt_counts
) {
    uint32_t snp_idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (snp_idx >= num_snps) return;

    uint64_t pos = snp_positions[snp_idx];
    char ref_base = reference[pos];

    uint32_t ref_count = 0;
    uint32_t alt_count = 0;

    // Iterate through alignments covering this position
    for (uint32_t i = 0; i < num_alignments; ++i) {
        const AmpliconAlignment& aln = alignments[i];

        // Check if alignment covers this position
        if (aln.position <= pos && pos < aln.position + aln.read_length) {
            char read_base = reads[aln.read_offset + (pos - aln.position)];

            if (read_base == ref_base) {
                ref_count += aln.read_count;  // Weight by collapsed count
            } else {
                alt_count += aln.read_count;
            }
        }
    }

    ref_counts[snp_idx] = ref_count;
    alt_counts[snp_idx] = alt_count;
}
```

---

## Configuration

### Input Format

```yaml
# amplicon_config.yaml

amplicon_panel:
  name: "Phylos Cannabis Genotyping Panel"
  version: "v2.0"
  reference: "cannabis_sativa_cs10.fasta"

targets:
  - amplicon_id: "PHYLOS_SNP_0001"
    chromosome: "chr1"
    start: 12345
    end: 12545
    primer_fwd: "ACGTACGTACGT"
    primer_rev: "GCTAGCTAGCTA"
    snp_positions: [12400, 12450]

  - amplicon_id: "PHYLOS_SNP_0002"
    chromosome: "chr2"
    start: 54321
    end: 54521
    primer_fwd: "TGCATGCATGCA"
    primer_rev: "ATCGATCGATCG"
    snp_positions: [54400]

processing:
  collapse_reads: true
  max_cluster_distance: 2
  min_depth: 100
  min_allele_frequency: 0.01
  quality_threshold: 20

output:
  format: "vcf"
  include_depth: true
  include_allele_frequencies: true
  per_amplicon_stats: true
```

### Output Format

**VCF with amplicon-specific annotations**:

```vcf
##fileformat=VCFv4.2
##source=WinAlign-Amplicon v0.1.0
##INFO=<ID=AMP,Number=1,Type=String,Description="Amplicon ID">
##INFO=<ID=DP,Number=1,Type=Integer,Description="Total depth (collapsed reads)">
##INFO=<ID=AF,Number=A,Type=Float,Description="Allele frequency">
##INFO=<ID=RD,Number=1,Type=Integer,Description="Reference depth">
##INFO=<ID=AD,Number=1,Type=Integer,Description="Alternate depth">
##FORMAT=<ID=GT,Number=1,Type=String,Description="Genotype">
##FORMAT=<ID=DP,Number=1,Type=Integer,Description="Read depth">
#CHROM  POS     ID              REF ALT QUAL FILTER INFO                                    FORMAT  sample_001
chr1    12400   PHYLOS_SNP_0001 A   G   60   PASS   AMP=PHYLOS_SNP_0001;DP=15234;AF=0.48;RD=7921;AD=7313  GT:DP   0/1:15234
chr2    54400   PHYLOS_SNP_0002 C   T   60   PASS   AMP=PHYLOS_SNP_0002;DP=22145;AF=0.99;RD=221;AD=21924   GT:DP   1/1:22145
```

---

## Performance Expectations

### Throughput Comparison

| Dataset | Reads | WGS Pipeline | Amplicon Pipeline | Speedup |
|---------|-------|--------------|-------------------|---------|
| 100 amplicons, 1M reads | 1M | 5 min | 30 sec | 10x |
| 500 amplicons, 10M reads | 10M | 50 min | 2 min | 25x |

**Why faster?**
- Read collapsing: 10,000 → 50 unique sequences
- No k-mer seeding overhead
- Direct amplicon assignment
- Smaller alignment windows
- No genome-wide duplicate detection needed

### Resource Usage

**Memory**:
- CPU: 4-8 GB (hash tables for collapsing)
- GPU: 2-4 GB (much smaller than WGS)

**GPU Utilization**:
- Lower than WGS (fewer unique sequences)
- But still beneficial for pileup generation
- Best for large sample batches (100+ samples)

---

## Implementation Roadmap

### Phase 1: Core Pipeline (Week 1-2)
- [ ] Read collapsing algorithm (CPU)
- [ ] Amplicon assignment logic
- [ ] Configuration file parser
- [ ] Basic alignment to known positions

### Phase 2: GPU Acceleration (Week 3)
- [ ] GPU pileup generation kernel
- [ ] Parallel variant calling
- [ ] Batch processing optimization

### Phase 3: Output & QC (Week 4)
- [ ] VCF writer with amplicon annotations
- [ ] Per-amplicon coverage statistics
- [ ] Quality control metrics
- [ ] Visualization tools

### Phase 4: Integration & Testing (Week 5-6)
- [ ] Unit tests for all components
- [ ] Integration tests with Phylos data
- [ ] Benchmarking vs existing tools (DADA2, etc.)
- [ ] Documentation and examples

---

## Comparison to Existing Tools

| Tool | Type | Speed | GPU | Amplicon-Optimized |
|------|------|-------|-----|-------------------|
| **DADA2** | R package | Slow | No | Yes (best) |
| **mothur** | CLI | Moderate | No | Yes |
| **BWA-MEM** | CLI | Fast | No | No (general purpose) |
| **WinAlign-GPU** | CLI | Very Fast | Yes | No (WGS optimized) |
| **WinAlign-Amplicon** | CLI | Very Fast | Yes | **Yes** |

**Competitive Advantage**:
- Only GPU-accelerated amplicon tool
- Integrated with WinAlign ecosystem
- Windows-native (like main pipeline)
- Faster than DADA2, more specialized than BWA-MEM

---

## Integration with Relatedness Analysis

This tool produces VCF output compatible with the relatedness analysis pipeline:

```bash
# Process Phylos amplicon data
winalign-amplicon -c phylos_panel.yaml -i phylos.fastq.gz -o phylos.vcf

# Process your panel (WGS)
winalign-gpu -r ref.fa -1 wgs_R1.fq -2 wgs_R2.fq -o wgs.bam
bcftools mpileup -f ref.fa wgs.bam | bcftools call -mv > wgs.vcf

# Run relatedness analysis
python calculate_relatedness.py --vcf1 phylos.vcf --vcf2 wgs.vcf
```

---

## Design Decisions & Rationale

### Why Separate from WinAlign-GPU?

1. **Different Algorithms**: K-mer seeding irrelevant for amplicons
2. **Different Bottlenecks**: Deduplication vs throughput
3. **Different Output**: VCF vs BAM (no need for full alignments)
4. **Code Clarity**: Mixing would add complexity to both pipelines

### Why CPU-Based Collapsing?

- Hash tables are CPU-efficient
- GPU transfer overhead not worth it for this step
- CPU can process while GPU aligns previous batch

### Why GPU Pileup?

- Thousands of positions × thousands of reads = parallel workload
- Depth counting is embarrassingly parallel
- GPU excels at aggregation operations

---

## Testing Strategy

### Unit Tests
- Read collapsing with known duplicates
- Amplicon assignment accuracy
- Pileup generation correctness
- VCF output validation

### Integration Tests
- End-to-end with synthetic amplicon data
- Phylos dataset processing
- Multi-sample batch processing

### Validation
- Compare to DADA2 on same dataset
- Genotype concordance checks
- Coverage uniformity analysis

---

## Future Extensions

### Potential Features
- [ ] Support for multiple amplicon panels
- [ ] Chimera detection
- [ ] Primer trimming
- [ ] Barcode demultiplexing
- [ ] Multi-GPU support for massive sample batches
- [ ] Real-time processing mode
- [ ] Cloud deployment (AWS, Azure)

---

## References

- Phylos Bioscience genotyping methodology
- DADA2: High-resolution sample inference from Illumina amplicon data
- BWA-MEM: Fast and accurate long-read alignment
- WinAlign-GPU architecture document

---

**Status**: Design phase complete, ready for implementation
**Owner**: WinAlign Development Team
**Last Updated**: 2025-11-15
