#include "winalign/amplicon/vcf_writer.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <ctime>

namespace winalign {
namespace amplicon {

class VCFWriter::Impl {
public:
    Impl(const std::string& output_path,
         const std::string& reference_path,
         const std::vector<std::string>& sample_names)
        : output_path_(output_path)
        , reference_path_(reference_path)
        , sample_names_(sample_names)
        , variant_count_(0)
    {}

    Result<bool> open() {
        out_.open(output_path_);
        if (!out_.is_open()) {
            return Result<bool>(ErrorCode::FILE_NOT_FOUND,
                              "Cannot open VCF file: " + output_path_);
        }
        return Result<bool>(true);
    }

    Result<bool> write_header(const std::vector<AmpliconTarget>& targets) {
        if (!out_.is_open()) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                              "VCF file not open");
        }

        // VCF version
        out_ << "##fileformat=VCFv4.2\n";

        // Date
        time_t now = time(nullptr);
        char date_buf[32];
        strftime(date_buf, sizeof(date_buf), "%Y%m%d", localtime(&now));
        out_ << "##fileDate=" << date_buf << "\n";

        // Source
        out_ << "##source=WinAlign-Amplicon v" << VERSION << "\n";

        // Reference
        out_ << "##reference=" << reference_path_ << "\n";

        // Contig information (from targets)
        std::set<std::string> chromosomes;
        for (const auto& target : targets) {
            chromosomes.insert(target.chromosome);
        }

        for (const auto& chr : chromosomes) {
            out_ << "##contig=<ID=" << chr << ">\n";
        }

        // INFO fields
        out_ << "##INFO=<ID=AMP,Number=1,Type=String,Description=\"Amplicon ID\">\n";
        out_ << "##INFO=<ID=DP,Number=1,Type=Integer,Description=\"Total depth (collapsed reads)\">\n";
        out_ << "##INFO=<ID=AF,Number=A,Type=Float,Description=\"Allele frequency\">\n";
        out_ << "##INFO=<ID=RD,Number=1,Type=Integer,Description=\"Reference allele depth\">\n";
        out_ << "##INFO=<ID=AD,Number=A,Type=Integer,Description=\"Alternate allele depth\">\n";
        out_ << "##INFO=<ID=KNOWN,Number=0,Type=Flag,Description=\"Known marker position\">\n";

        // FORMAT fields
        out_ << "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n";
        out_ << "##FORMAT=<ID=DP,Number=1,Type=Integer,Description=\"Read depth\">\n";
        out_ << "##FORMAT=<ID=AD,Number=R,Type=Integer,Description=\"Allelic depths (ref,alt)\">\n";
        out_ << "##FORMAT=<ID=AF,Number=A,Type=Float,Description=\"Allele frequency\">\n";

        // FILTER fields
        out_ << "##FILTER=<ID=PASS,Description=\"All filters passed\">\n";
        out_ << "##FILTER=<ID=LowDepth,Description=\"Depth below minimum threshold\">\n";
        out_ << "##FILTER=<ID=LowAF,Description=\"Allele frequency below threshold\">\n";

        // Column header
        out_ << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT";

        // Sample names
        for (const auto& sample : sample_names_) {
            out_ << "\t" << sample;
        }
        out_ << "\n";

        out_.flush();

        return Result<bool>(true);
    }

    Result<bool> write_variants(const std::vector<AmpliconVariant>& variants) {
        for (const auto& variant : variants) {
            auto result = write_variant(variant);
            if (!result.is_ok()) {
                return result;
            }
        }
        return Result<bool>(true);
    }

    Result<bool> write_variant(const AmpliconVariant& variant) {
        if (!out_.is_open()) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                              "VCF file not open");
        }

        // CHROM
        out_ << variant.chromosome << "\t";

        // POS (1-based in VCF)
        out_ << (variant.position + 1) << "\t";

        // ID (use amplicon_id if available)
        if (!variant.amplicon_id.empty()) {
            out_ << variant.amplicon_id;
        } else {
            out_ << ".";
        }
        out_ << "\t";

        // REF
        out_ << variant.ref_allele << "\t";

        // ALT
        out_ << variant.alt_allele << "\t";

        // QUAL
        out_ << static_cast<int>(variant.quality) << "\t";

        // FILTER
        out_ << "PASS\t";

        // INFO
        std::stringstream info;
        if (!variant.amplicon_id.empty()) {
            info << "AMP=" << variant.amplicon_id << ";";
        }
        info << "DP=" << variant.total_depth << ";";
        info << "AF=" << std::fixed << std::setprecision(3) << variant.allele_frequency << ";";
        info << "RD=" << variant.ref_depth << ";";
        info << "AD=" << variant.alt_depth;
        if (variant.is_known_marker) {
            info << ";KNOWN";
        }

        out_ << info.str() << "\t";

        // FORMAT
        out_ << "GT:DP:AD:AF\t";

        // Sample genotypes
        for (size_t i = 0; i < sample_names_.size(); ++i) {
            // Genotype
            std::string gt = variant.genotype();
            out_ << gt << ":";

            // Depth
            out_ << variant.total_depth << ":";

            // Allelic depths
            out_ << variant.ref_depth << "," << variant.alt_depth << ":";

            // Allele frequency
            out_ << std::fixed << std::setprecision(3) << variant.allele_frequency;

            if (i < sample_names_.size() - 1) {
                out_ << "\t";
            }
        }

        out_ << "\n";

        variant_count_++;

        return Result<bool>(true);
    }

    void close() {
        if (out_.is_open()) {
            out_.close();
        }
    }

    uint64_t get_variant_count() const {
        return variant_count_;
    }

private:
    std::string output_path_;
    std::string reference_path_;
    std::vector<std::string> sample_names_;
    std::ofstream out_;
    uint64_t variant_count_;
};

// Public interface
VCFWriter::VCFWriter(const std::string& output_path,
                     const std::string& reference_path,
                     const std::vector<std::string>& sample_names)
    : pimpl_(std::make_unique<Impl>(output_path, reference_path, sample_names))
{}

VCFWriter::~VCFWriter() {
    if (pimpl_) {
        pimpl_->close();
    }
}

Result<bool> VCFWriter::open() {
    return pimpl_->open();
}

Result<bool> VCFWriter::write_header(const std::vector<AmpliconTarget>& targets) {
    return pimpl_->write_header(targets);
}

Result<bool> VCFWriter::write_variants(const std::vector<AmpliconVariant>& variants) {
    return pimpl_->write_variants(variants);
}

Result<bool> VCFWriter::write_variant(const AmpliconVariant& variant) {
    return pimpl_->write_variant(variant);
}

void VCFWriter::close() {
    pimpl_->close();
}

uint64_t VCFWriter::get_variant_count() const {
    return pimpl_->get_variant_count();
}

} // namespace amplicon
} // namespace winalign
