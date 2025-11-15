#ifndef WINALIGN_AMPLICON_VCF_WRITER_H
#define WINALIGN_AMPLICON_VCF_WRITER_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>
#include <vector>
#include <memory>

namespace winalign {
namespace amplicon {

/**
 * VCF Writer for Amplicon Variants
 *
 * Writes variants in standard VCF format with amplicon-specific annotations.
 */
class VCFWriter {
public:
    VCFWriter(const std::string& output_path,
              const std::string& reference_path,
              const std::vector<std::string>& sample_names);
    ~VCFWriter();

    /**
     * Open VCF file for writing
     */
    Result<bool> open();

    /**
     * Write VCF header
     * Includes standard VCF headers plus amplicon-specific INFO fields
     */
    Result<bool> write_header(const std::vector<AmpliconTarget>& targets);

    /**
     * Write a batch of variants
     */
    Result<bool> write_variants(const std::vector<AmpliconVariant>& variants);

    /**
     * Write a single variant
     */
    Result<bool> write_variant(const AmpliconVariant& variant);

    /**
     * Close VCF file
     */
    void close();

    /**
     * Get total variants written
     */
    uint64_t get_variant_count() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_VCF_WRITER_H
