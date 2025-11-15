#include "winalign/amplicon/multi_sample_vcf.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <ctime>

namespace winalign {
namespace amplicon {

/**
 * Implementation of multi-sample VCF writer
 */
class MultiSampleVCFWriter::Impl {
public:
    std::string output_path_;
    std::string reference_path_;
    std::vector<std::string> sample_names_;
    std::ofstream out_;
    bool streaming_enabled_;
    Stats stats_;

    Impl(const std::string& output_path,
         const std::string& reference_path,
         const std::vector<std::string>& sample_names)
        : output_path_(output_path),
          reference_path_(reference_path),
          sample_names_(sample_names),
          streaming_enabled_(false) {
        stats_ = Stats{0, 0, 0, {}};
    }

    ~Impl() {
        if (out_.is_open()) {
            out_.close();
        }
    }

    Result<bool> open() {
        out_.open(output_path_);
        if (!out_.is_open()) {
            return Result<bool>::error("Failed to open VCF file: " + output_path_);
        }
        return Result<bool>::ok(true);
    }

    Result<bool> write_header(const std::vector<AmpliconTarget>& targets) {
        if (!out_.is_open()) {
            return Result<bool>::error("VCF file not open");
        }

        // Write VCF version
        out_ << "##fileformat=VCFv4.2\n";

        // Write date
        std::time_t now = std::time(nullptr);
        char date_buf[32];
        std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", std::localtime(&now));
        out_ << "##fileDate=" << date_buf << "\n";

        // Write source
        out_ << "##source=WinAlign-Amplicon v" << VERSION << "\n";

        // Write reference
        out_ << "##reference=" << reference_path_ << "\n";

        // Write INFO fields
        out_ << "##INFO=<ID=AMP,Number=1,Type=String,Description=\"Amplicon ID\">\n";
        out_ << "##INFO=<ID=AC,Number=A,Type=Integer,Description=\"Allele count in genotypes\">\n";
        out_ << "##INFO=<ID=AN,Number=1,Type=Integer,Description=\"Total number of alleles\">\n";
        out_ << "##INFO=<ID=AF,Number=A,Type=Float,Description=\"Allele frequency\">\n";
        out_ << "##INFO=<ID=DP,Number=1,Type=Integer,Description=\"Combined depth across samples\">\n";
        out_ << "##INFO=<ID=KNOWN,Number=0,Type=Flag,Description=\"Known marker position\">\n";

        // Write FORMAT fields
        out_ << "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n";
        out_ << "##FORMAT=<ID=DP,Number=1,Type=Integer,Description=\"Read depth\">\n";
        out_ << "##FORMAT=<ID=AD,Number=R,Type=Integer,Description=\"Allelic depths (ref,alt)\">\n";
        out_ << "##FORMAT=<ID=AF,Number=A,Type=Float,Description=\"Allele frequency\">\n";
        out_ << "##FORMAT=<ID=GQ,Number=1,Type=Integer,Description=\"Genotype quality\">\n";

        // Write contig lines
        std::set<std::string> chromosomes;
        for (const auto& target : targets) {
            chromosomes.insert(target.chromosome);
        }

        for (const auto& chr : chromosomes) {
            out_ << "##contig=<ID=" << chr << ">\n";
        }

        // Write column header
        out_ << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT";

        // Write sample columns
        for (const auto& sample : sample_names_) {
            out_ << "\t" << sample;
        }
        out_ << "\n";

        return Result<bool>::ok(true);
    }

    Result<bool> write_variant(const MultiSampleVariant& variant) {
        if (!out_.is_open()) {
            return Result<bool>::error("VCF file not open");
        }

        // Calculate aggregate statistics
        uint32_t total_depth = 0;
        uint32_t alt_count = 0;
        uint32_t allele_count = 0;

        for (const auto& sample : sample_names_) {
            auto it = variant.sample_genotypes.find(sample);
            if (it != variant.sample_genotypes.end()) {
                const auto& gt = it->second;
                total_depth += gt.depth;

                // Count alleles
                if (gt.genotype == "0/0") {
                    allele_count += 2;
                } else if (gt.genotype == "0/1" || gt.genotype == "1/0") {
                    allele_count += 2;
                    alt_count += 1;
                } else if (gt.genotype == "1/1") {
                    allele_count += 2;
                    alt_count += 2;
                }

                stats_.total_genotypes++;
            } else {
                stats_.missing_genotypes++;
            }
        }

        float af = (allele_count > 0) ? (static_cast<float>(alt_count) / allele_count) : 0.0f;

        // Write fixed fields
        out_ << variant.chromosome << "\t"
             << variant.position << "\t"
             << variant.amplicon_id << "\t"
             << variant.ref_allele << "\t"
             << variant.alt_allele << "\t"
             << "60\t"  // Quality (use max for multi-sample)
             << "PASS\t";

        // Write INFO field
        out_ << "AMP=" << variant.amplicon_id << ";"
             << "AC=" << alt_count << ";"
             << "AN=" << allele_count << ";"
             << "AF=" << std::fixed << std::setprecision(4) << af << ";"
             << "DP=" << total_depth;

        if (variant.is_known_marker) {
            out_ << ";KNOWN";
        }

        // Write FORMAT field
        out_ << "\tGT:DP:AD:AF:GQ";

        // Write sample genotypes
        for (const auto& sample : sample_names_) {
            auto it = variant.sample_genotypes.find(sample);

            if (it != variant.sample_genotypes.end()) {
                const auto& gt = it->second;
                out_ << "\t" << gt.genotype
                     << ":" << gt.depth
                     << ":" << gt.ref_depth << "," << gt.alt_depth
                     << ":" << std::fixed << std::setprecision(3) << gt.allele_frequency
                     << ":" << static_cast<int>(gt.quality);

                stats_.per_sample_variant_counts[sample]++;
            } else {
                // Missing genotype
                out_ << "\t./.:0:0,0:0.000:0";
            }
        }

        out_ << "\n";

        stats_.variants_written++;

        if (streaming_enabled_) {
            out_.flush();
        }

        return Result<bool>::ok(true);
    }

    Result<bool> flush() {
        if (out_.is_open()) {
            out_.flush();
        }
        return Result<bool>::ok(true);
    }
};

// Public interface

MultiSampleVCFWriter::MultiSampleVCFWriter(
    const std::string& output_path,
    const std::string& reference_path,
    const std::vector<std::string>& sample_names
) : pimpl_(std::make_unique<Impl>(output_path, reference_path, sample_names)) {}

MultiSampleVCFWriter::~MultiSampleVCFWriter() = default;

Result<bool> MultiSampleVCFWriter::open() {
    return pimpl_->open();
}

Result<bool> MultiSampleVCFWriter::write_header(const std::vector<AmpliconTarget>& targets) {
    return pimpl_->write_header(targets);
}

Result<bool> MultiSampleVCFWriter::write_variant(const MultiSampleVariant& variant) {
    return pimpl_->write_variant(variant);
}

Result<bool> MultiSampleVCFWriter::write_variants(const std::vector<MultiSampleVariant>& variants) {
    for (const auto& variant : variants) {
        auto result = write_variant(variant);
        if (!result.is_ok()) {
            return result;
        }
    }
    return Result<bool>::ok(true);
}

void MultiSampleVCFWriter::enable_streaming(bool enable) {
    pimpl_->streaming_enabled_ = enable;
}

Result<bool> MultiSampleVCFWriter::flush() {
    return pimpl_->flush();
}

void MultiSampleVCFWriter::close() {
    if (pimpl_->out_.is_open()) {
        pimpl_->out_.close();
    }
}

MultiSampleVCFWriter::Stats MultiSampleVCFWriter::get_stats() const {
    return pimpl_->stats_;
}

// Utility functions

MultiSampleVariant merge_variants(
    const std::string& amplicon_id,
    const std::string& chromosome,
    uint64_t position,
    const std::unordered_map<std::string, AmpliconVariant>& per_sample_variants
) {
    MultiSampleVariant result;
    result.amplicon_id = amplicon_id;
    result.chromosome = chromosome;
    result.position = position;

    // Use first variant to get ref/alt alleles
    if (!per_sample_variants.empty()) {
        const auto& first_var = per_sample_variants.begin()->second;
        result.ref_allele = first_var.ref_allele;
        result.alt_allele = first_var.alt_allele;
        result.is_known_marker = first_var.is_known_marker;
    }

    // Merge genotypes from all samples
    for (const auto& [sample_id, variant] : per_sample_variants) {
        MultiSampleVariant::SampleGenotype gt;
        gt.genotype = variant.genotype();
        gt.depth = variant.total_depth;
        gt.ref_depth = variant.ref_depth;
        gt.alt_depth = variant.alt_depth;
        gt.allele_frequency = variant.allele_frequency;
        gt.quality = variant.quality;

        result.sample_genotypes[sample_id] = gt;
    }

    return result;
}

// Joint genotyper implementation

class JointGenotyper::Impl {
public:
    std::unordered_map<uint64_t, float> position_priors_;

    std::vector<MultiSampleVariant> joint_genotype(
        const std::vector<AmpliconTarget>& targets,
        const std::unordered_map<std::string, std::vector<AmpliconVariant>>& per_sample_variants
    ) {
        std::vector<MultiSampleVariant> joint_variants;

        // Collect all unique positions across samples
        std::set<uint64_t> all_positions;
        for (const auto& [sample_id, variants] : per_sample_variants) {
            for (const auto& var : variants) {
                all_positions.insert(var.position);
            }
        }

        // For each position, create a multi-sample variant
        for (uint64_t pos : all_positions) {
            std::unordered_map<std::string, AmpliconVariant> pos_variants;
            std::string amplicon_id;
            std::string chromosome;

            for (const auto& [sample_id, variants] : per_sample_variants) {
                for (const auto& var : variants) {
                    if (var.position == pos) {
                        pos_variants[sample_id] = var;
                        amplicon_id = var.amplicon_id;
                        chromosome = var.chromosome;
                        break;
                    }
                }
            }

            if (!pos_variants.empty()) {
                auto joint_var = merge_variants(amplicon_id, chromosome, pos, pos_variants);
                joint_variants.push_back(joint_var);
            }
        }

        return joint_variants;
    }
};

JointGenotyper::JointGenotyper()
    : pimpl_(std::make_unique<Impl>()) {}

JointGenotyper::~JointGenotyper() = default;

std::vector<MultiSampleVariant> JointGenotyper::joint_genotype(
    const std::vector<AmpliconTarget>& targets,
    const std::unordered_map<std::string, std::vector<AmpliconVariant>>& per_sample_variants
) {
    return pimpl_->joint_genotype(targets, per_sample_variants);
}

void JointGenotyper::set_population_priors(
    const std::unordered_map<uint64_t, float>& position_to_af
) {
    pimpl_->position_priors_ = position_to_af;
}

} // namespace amplicon
} // namespace winalign
