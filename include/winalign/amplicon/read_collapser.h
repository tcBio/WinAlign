#ifndef WINALIGN_AMPLICON_READ_COLLAPSER_H
#define WINALIGN_AMPLICON_READ_COLLAPSER_H

#include "amplicon_types.h"
#include <unordered_map>
#include <vector>

namespace winalign {
namespace amplicon {

/**
 * Read Collapser
 *
 * Collapses identical or near-identical reads into clusters.
 * This is the KEY optimization for amplicon data.
 *
 * Example: 10,000 reads from same 200bp amplicon
 *   -> Collapse to ~50-500 unique sequences
 *   -> 20-200x reduction in downstream processing
 */
class ReadCollapser {
public:
    ReadCollapser(uint32_t max_distance = 2);
    ~ReadCollapser();

    /**
     * Collapse reads into clusters
     *
     * @param reads Input reads
     * @return Vector of read clusters
     */
    std::vector<ReadCluster> collapse(const std::vector<Read>& reads);

    /**
     * Get statistics about collapsing efficiency
     */
    struct CollapseStats {
        uint32_t input_reads;
        uint32_t output_clusters;
        float compression_ratio;  // input / output
        float avg_cluster_size;
    };

    CollapseStats get_stats() const { return stats_; }

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    CollapseStats stats_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_READ_COLLAPSER_H
