#ifndef WINALIGN_AMPLICON_CHIMERA_DETECTOR_H
#define WINALIGN_AMPLICON_CHIMERA_DETECTOR_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>
#include <vector>

namespace winalign {
namespace amplicon {

/**
 * Chimera detection parameters
 */
struct ChimeraParams {
    float min_abundance_ratio;      // Min parent/query abundance ratio (default: 2.0)
    uint32_t min_segment_length;    // Min segment length for split (default: 30)
    uint32_t min_score_diff;        // Min score difference for chimera call (default: 10)
    bool use_de_novo;               // Use de novo detection (default: true)
    bool use_reference;             // Use reference-based detection (default: true)

    ChimeraParams()
        : min_abundance_ratio(2.0f), min_segment_length(30),
          min_score_diff(10), use_de_novo(true), use_reference(true) {}
};

/**
 * Chimera detection result
 */
struct ChimeraResult {
    bool is_chimera;                // True if sequence is chimeric
    std::string parent_left;        // Left parent sequence ID
    std::string parent_right;       // Right parent sequence ID
    uint32_t breakpoint;            // Estimated breakpoint position
    float confidence;               // Confidence score (0-1)
    std::string method;             // Detection method used

    ChimeraResult()
        : is_chimera(false), breakpoint(0), confidence(0.0f), method("none") {}
};

/**
 * Chimera detector for amplicon sequences
 *
 * Detects PCR chimeras using both de novo and reference-based methods.
 * Critical QC step for amplicon sequencing to remove artifacts.
 */
class ChimeraDetector {
public:
    ChimeraDetector(const ChimeraParams& params = ChimeraParams());
    ~ChimeraDetector();

    /**
     * Set reference sequences (amplicon targets)
     */
    void set_references(const std::vector<std::string>& references);

    /**
     * Add de novo reference from read clusters
     * Uses high-abundance clusters as references
     */
    void add_de_novo_references(const std::vector<ReadCluster>& clusters);

    /**
     * Check if a sequence is chimeric
     *
     * @param sequence Query sequence
     * @param abundance Abundance/read count of this sequence
     * @return Chimera detection result
     */
    ChimeraResult detect(const std::string& sequence, uint32_t abundance);

    /**
     * Check read cluster for chimeras
     */
    ChimeraResult detect_cluster(const ReadCluster& cluster);

    /**
     * Filter chimeras from cluster list
     * Returns non-chimeric clusters
     */
    std::vector<ReadCluster> filter_chimeras(
        const std::vector<ReadCluster>& clusters
    );

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t total_checked;
        uint64_t chimeras_found;
        uint64_t de_novo_detected;
        uint64_t reference_detected;
        float chimera_rate;
    };

    Stats get_stats() const;
    void reset_stats();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * De novo chimera detection using UCHIME algorithm
 * Compares query to potential parent sequences
 */
bool detect_chimera_de_novo(
    const std::string& query,
    const std::vector<std::string>& references,
    const std::vector<uint32_t>& abundances,
    uint32_t query_abundance,
    const ChimeraParams& params,
    uint32_t& breakpoint
);

/**
 * Reference-based chimera detection
 * Checks if query is a chimera of two known amplicons
 */
bool detect_chimera_reference(
    const std::string& query,
    const std::vector<std::string>& references,
    const ChimeraParams& params,
    uint32_t& breakpoint
);

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_CHIMERA_DETECTOR_H
