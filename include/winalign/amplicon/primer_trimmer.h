#ifndef WINALIGN_AMPLICON_PRIMER_TRIMMER_H
#define WINALIGN_AMPLICON_PRIMER_TRIMMER_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>
#include <vector>

namespace winalign {
namespace amplicon {

/**
 * Primer trimming parameters
 */
struct PrimerTrimParams {
    uint32_t max_mismatches;        // Max mismatches allowed (default: 2)
    uint32_t min_overlap;           // Min overlap for detection (default: 10)
    bool trim_both_ends;            // Trim both forward and reverse primers
    bool check_reverse_complement;  // Check RC of primers

    PrimerTrimParams()
        : max_mismatches(2), min_overlap(10),
          trim_both_ends(true), check_reverse_complement(true) {}
};

/**
 * Primer trimming result
 */
struct PrimerTrimResult {
    std::string trimmed_sequence;   // Sequence after trimming
    std::string trimmed_quality;    // Quality scores after trimming
    uint32_t fwd_trim_len;          // Bases trimmed from 5' end
    uint32_t rev_trim_len;          // Bases trimmed from 3' end
    bool fwd_found;                 // Forward primer detected
    bool rev_found;                 // Reverse primer detected
    std::string amplicon_id;        // Matched amplicon ID

    PrimerTrimResult()
        : fwd_trim_len(0), rev_trim_len(0),
          fwd_found(false), rev_found(false) {}

    bool any_trimmed() const {
        return fwd_trim_len > 0 || rev_trim_len > 0;
    }
};

/**
 * Primer trimmer for amplicon reads
 *
 * Detects and removes primer sequences from reads.
 * Critical preprocessing step for amplicon sequencing.
 */
class PrimerTrimmer {
public:
    PrimerTrimmer(const PrimerTrimParams& params = PrimerTrimParams());
    ~PrimerTrimmer();

    /**
     * Set amplicon targets with primers
     */
    void set_targets(const std::vector<AmpliconTarget>& targets);

    /**
     * Trim primers from a single read
     *
     * @param sequence Read sequence
     * @param quality Quality scores
     * @return Trimming result with trimmed sequence
     */
    PrimerTrimResult trim(
        const std::string& sequence,
        const std::string& quality
    );

    /**
     * Trim primers from read cluster
     */
    PrimerTrimResult trim_cluster(ReadCluster& cluster);

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t total_reads;
        uint64_t fwd_primer_found;
        uint64_t rev_primer_found;
        uint64_t both_found;
        uint64_t neither_found;
        uint64_t total_bases_trimmed;
    };

    Stats get_stats() const;
    void reset_stats();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Reverse complement a DNA sequence
 */
std::string reverse_complement(const std::string& seq);

/**
 * Find primer in sequence with mismatches allowed
 * Returns position and number of mismatches, or -1 if not found
 */
std::pair<int32_t, uint32_t> find_primer(
    const std::string& sequence,
    const std::string& primer,
    uint32_t max_mismatches,
    uint32_t min_overlap
);

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_PRIMER_TRIMMER_H
