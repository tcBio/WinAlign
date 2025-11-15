#ifndef WINALIGN_AMPLICON_MULTI_SAMPLE_VCF_H
#define WINALIGN_AMPLICON_MULTI_SAMPLE_VCF_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace winalign {
namespace amplicon {

/**
 * Multi-sample variant record
 * Stores genotype information for all samples at a position
 */
struct MultiSampleVariant {
    std::string amplicon_id;        // Source amplicon
    std::string chromosome;         // Chromosome
    uint64_t position;              // Genomic position
    char ref_allele;                // Reference allele
    char alt_allele;                // Alternate allele
    bool is_known_marker;           // Expected variant position

    // Per-sample genotype data
    struct SampleGenotype {
        std::string genotype;       // GT: "0/0", "0/1", "1/1", "./."
        uint32_t depth;             // DP: Total depth
        uint32_t ref_depth;         // Ref allele depth
        uint32_t alt_depth;         // Alt allele depth
        float allele_frequency;     // AF: Alt allele frequency
        uint8_t quality;            // GQ: Genotype quality

        SampleGenotype()
            : genotype("./."), depth(0), ref_depth(0), alt_depth(0),
              allele_frequency(0.0f), quality(0) {}
    };

    std::unordered_map<std::string, SampleGenotype> sample_genotypes;

    MultiSampleVariant()
        : position(0), ref_allele('N'), alt_allele('N'),
          is_known_marker(false) {}
};

/**
 * Multi-sample VCF writer
 * Handles joint variant calling across multiple samples
 */
class MultiSampleVCFWriter {
public:
    MultiSampleVCFWriter(
        const std::string& output_path,
        const std::string& reference_path,
        const std::vector<std::string>& sample_names
    );
    ~MultiSampleVCFWriter();

    /**
     * Open VCF file for writing
     */
    Result<bool> open();

    /**
     * Write VCF header with multi-sample FORMAT fields
     */
    Result<bool> write_header(const std::vector<AmpliconTarget>& targets);

    /**
     * Write a multi-sample variant record
     */
    Result<bool> write_variant(const MultiSampleVariant& variant);

    /**
     * Write batch of multi-sample variants
     */
    Result<bool> write_variants(const std::vector<MultiSampleVariant>& variants);

    /**
     * Enable streaming mode (write variants as they arrive)
     */
    void enable_streaming(bool enable);

    /**
     * Flush buffered variants to disk
     */
    Result<bool> flush();

    /**
     * Close VCF file
     */
    void close();

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t variants_written;
        uint64_t total_genotypes;
        uint64_t missing_genotypes;
        std::unordered_map<std::string, uint64_t> per_sample_variant_counts;
    };

    Stats get_stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Merge single-sample variants into multi-sample variant
 */
MultiSampleVariant merge_variants(
    const std::string& amplicon_id,
    const std::string& chromosome,
    uint64_t position,
    const std::unordered_map<std::string, AmpliconVariant>& per_sample_variants
);

/**
 * Joint genotyper for multi-sample data
 * Re-calls genotypes considering all samples together
 */
class JointGenotyper {
public:
    JointGenotyper();
    ~JointGenotyper();

    /**
     * Perform joint genotyping across samples
     * Uses allele frequencies from all samples to improve calling
     */
    std::vector<MultiSampleVariant> joint_genotype(
        const std::vector<AmpliconTarget>& targets,
        const std::unordered_map<std::string, std::vector<AmpliconVariant>>& per_sample_variants
    );

    /**
     * Calculate prior allele frequencies from population
     */
    void set_population_priors(
        const std::unordered_map<uint64_t, float>& position_to_af
    );

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_MULTI_SAMPLE_VCF_H
