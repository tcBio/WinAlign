#ifndef WINALIGN_AMPLICON_TYPES_H
#define WINALIGN_AMPLICON_TYPES_H

#include "winalign/common.h"
#include <vector>
#include <string>
#include <cstdint>

namespace winalign {
namespace amplicon {

/**
 * Amplicon target definition
 * Represents a single amplicon region with primers and expected SNP positions
 */
struct AmpliconTarget {
    std::string amplicon_id;              // Unique identifier (e.g., "MARKER_0001")
    std::string chromosome;               // Chromosome name
    uint64_t start;                       // 0-based start position
    uint64_t end;                         // 0-based end position
    std::string primer_fwd;               // Forward primer sequence
    std::string primer_rev;               // Reverse primer sequence
    uint32_t expected_length;             // Expected amplicon length
    std::vector<uint64_t> snp_positions;  // Known SNP positions within amplicon

    AmpliconTarget() : start(0), end(0), expected_length(0) {}

    uint32_t length() const { return end - start; }
    bool is_valid() const {
        return !amplicon_id.empty() && !chromosome.empty() && start < end;
    }
};

/**
 * Read cluster - represents collapsed identical/similar reads
 * Key optimization: reduce 10,000 identical reads to 1 representative
 */
struct ReadCluster {
    std::string consensus_sequence;       // Representative sequence
    std::string consensus_quality;        // Merged quality scores
    uint32_t read_count;                  // Number of collapsed reads
    std::vector<ReadID> read_ids;         // Original read identifiers
    float avg_quality;                    // Average quality score
    std::string amplicon_id;              // Assigned amplicon
    uint64_t hash;                        // Sequence hash for fast lookup

    ReadCluster() : read_count(0), avg_quality(0.0f), hash(0) {}

    bool empty() const { return consensus_sequence.empty(); }
    size_t length() const { return consensus_sequence.length(); }
};

/**
 * Amplicon-specific alignment result
 * Unlike WGS, we track read counts and amplicon assignment
 */
struct AmpliconAlignment {
    std::string amplicon_id;              // Which amplicon
    uint64_t position;                    // Alignment position
    std::string cigar;                    // CIGAR string
    uint32_t read_count;                  // Collapsed read count (weight)
    uint32_t read_length;                 // Aligned read length
    uint32_t read_offset;                 // Offset in read batch
    uint8_t mapping_quality;              // MAPQ
    bool is_expected_position;            // Aligns to expected target?
    int32_t alignment_score;              // Smith-Waterman score

    AmpliconAlignment()
        : position(0), read_count(0), read_length(0), read_offset(0),
          mapping_quality(0), is_expected_position(false), alignment_score(0) {}
};

/**
 * Variant call specific to amplicon sequencing
 * Includes depth information critical for amplicon QC
 */
struct AmpliconVariant {
    std::string amplicon_id;              // Source amplicon
    std::string chromosome;               // Chromosome name
    uint64_t position;                    // Genomic position
    char ref_allele;                      // Reference base
    char alt_allele;                      // Alternate base
    uint32_t ref_depth;                   // Reads supporting reference
    uint32_t alt_depth;                   // Reads supporting alternate
    float allele_frequency;               // alt_depth / total_depth
    uint8_t quality;                      // Variant quality score
    bool is_known_marker;                 // Expected variant position?
    uint32_t total_depth;                 // Total coverage at position

    AmpliconVariant()
        : position(0), ref_allele('N'), alt_allele('N'),
          ref_depth(0), alt_depth(0), allele_frequency(0.0f),
          quality(0), is_known_marker(false), total_depth(0) {}

    // Calculate genotype from allele frequency
    std::string genotype() const {
        if (allele_frequency < 0.25) return "0/0";  // Homozygous ref
        if (allele_frequency > 0.75) return "1/1";  // Homozygous alt
        return "0/1";                                // Heterozygous
    }
};

/**
 * Per-amplicon statistics for QC
 */
struct AmpliconStats {
    std::string amplicon_id;
    uint32_t total_reads;                 // Raw reads assigned
    uint32_t unique_sequences;            // After collapsing
    uint32_t aligned_reads;               // Successfully aligned
    float mean_coverage;                  // Average depth
    float coverage_uniformity;            // Coefficient of variation
    uint32_t variants_called;             // Number of variants
    float alignment_rate;                 // Aligned / total

    AmpliconStats()
        : total_reads(0), unique_sequences(0), aligned_reads(0),
          mean_coverage(0.0f), coverage_uniformity(0.0f),
          variants_called(0), alignment_rate(0.0f) {}
};

/**
 * Configuration for amplicon pipeline
 */
struct AmpliconConfig {
    std::string panel_name;               // Panel identifier
    std::string reference_fasta;          // Reference genome
    std::vector<AmpliconTarget> targets;  // Amplicon definitions

    // Processing parameters
    bool collapse_reads;                  // Enable read collapsing
    uint32_t max_cluster_distance;        // Max distance for clustering (bp)
    uint32_t min_depth;                   // Minimum depth for variant calling
    float min_allele_frequency;           // Minimum AF for variant calling
    uint8_t quality_threshold;            // Minimum base quality

    // I/O
    std::string input_fastq;              // Input FASTQ file
    std::string output_vcf;               // Output VCF file
    std::string output_stats;             // QC statistics file

    // GPU settings
    uint32_t gpu_device_id;               // GPU device
    uint32_t batch_size;                  // Batch size for processing

    AmpliconConfig()
        : collapse_reads(true), max_cluster_distance(2),
          min_depth(100), min_allele_frequency(0.01f),
          quality_threshold(20), gpu_device_id(0),
          batch_size(10000) {}

    bool is_valid() const {
        return !reference_fasta.empty() && !input_fastq.empty() &&
               !output_vcf.empty() && !targets.empty();
    }
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_TYPES_H
