#ifndef WINALIGN_AMPLICON_SMITH_WATERMAN_H
#define WINALIGN_AMPLICON_SMITH_WATERMAN_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>
#include <vector>

namespace winalign {
namespace amplicon {

/**
 * Smith-Waterman alignment parameters
 */
struct SWParams {
    int32_t match_score;       // Default: 2
    int32_t mismatch_penalty;  // Default: -3
    int32_t gap_open;          // Default: -5
    int32_t gap_extend;        // Default: -2

    SWParams()
        : match_score(2), mismatch_penalty(-3),
          gap_open(-5), gap_extend(-2) {}
};

/**
 * Smith-Waterman alignment result
 */
struct SWAlignment {
    uint64_t ref_start;        // Start position in reference
    uint64_t ref_end;          // End position in reference
    uint32_t query_start;      // Start position in query
    uint32_t query_end;        // End position in query
    int32_t score;             // Alignment score
    std::string cigar;         // CIGAR string
    uint32_t matches;          // Number of matches
    uint32_t mismatches;       // Number of mismatches
    uint32_t gaps;             // Number of gaps
    float identity;            // Percent identity

    SWAlignment()
        : ref_start(0), ref_end(0), query_start(0), query_end(0),
          score(0), matches(0), mismatches(0), gaps(0), identity(0.0f) {}

    bool is_valid() const { return score > 0; }
};

/**
 * Smith-Waterman aligner for amplicon sequences
 *
 * Performs local alignment of reads to amplicon target regions.
 * Optimized for short amplicon sequences (100-500bp).
 */
class SmithWatermanAligner {
public:
    SmithWatermanAligner(const SWParams& params = SWParams());
    ~SmithWatermanAligner();

    /**
     * Align a read to an amplicon target region
     *
     * @param query Read sequence
     * @param target Amplicon reference sequence
     * @return Alignment result
     */
    SWAlignment align(const std::string& query, const std::string& target);

    /**
     * Align a read to multiple amplicon targets
     * Returns the best alignment
     *
     * @param query Read sequence
     * @param targets Vector of amplicon targets
     * @param target_sequences Reference sequences for each target
     * @return Best alignment and target index
     */
    std::pair<SWAlignment, int> align_to_best_target(
        const std::string& query,
        const std::vector<AmpliconTarget>& targets,
        const std::vector<std::string>& target_sequences
    );

    /**
     * Set alignment parameters
     */
    void set_params(const SWParams& params);

    /**
     * Get current parameters
     */
    SWParams get_params() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Generate CIGAR string from alignment traceback
 */
std::string generate_cigar(
    const std::string& query,
    const std::string& target,
    const std::vector<char>& traceback
);

/**
 * Calculate alignment identity percentage
 */
float calculate_identity(
    uint32_t matches,
    uint32_t mismatches,
    uint32_t gaps
);

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_SMITH_WATERMAN_H
