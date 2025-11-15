#ifndef WINALIGN_AMPLICON_VARIANT_CALLER_H
#define WINALIGN_AMPLICON_VARIANT_CALLER_H

#include "amplicon_types.h"
#include <vector>

namespace winalign {
namespace amplicon {

/**
 * Variant Caller for Amplicon Data
 *
 * Calls variants at known marker positions using pileup data.
 * Optimized for high-coverage amplicon sequencing.
 */
class VariantCaller {
public:
    VariantCaller(const AmpliconConfig& config);
    ~VariantCaller();

    /**
     * Call variants from alignments
     *
     * @param alignments Amplicon alignments
     * @param reference Reference sequence
     * @return Vector of called variants
     */
    std::vector<AmpliconVariant> call_variants(
        const std::vector<AmpliconAlignment>& alignments,
        const std::string& reference
    );

    /**
     * Get per-amplicon statistics
     */
    std::vector<AmpliconStats> get_amplicon_stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_VARIANT_CALLER_H
