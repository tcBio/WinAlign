#ifndef WINALIGN_AMPLICON_ASSIGNER_H
#define WINALIGN_AMPLICON_ASSIGNER_H

#include "amplicon_types.h"
#include <vector>
#include <string>

namespace winalign {
namespace amplicon {

/**
 * Amplicon Assigner
 *
 * Assigns reads to specific amplicons based on:
 * 1. Primer sequence matching (fast)
 * 2. Expected position alignment (fallback)
 *
 * This REPLACES k-mer seeding from WGS pipeline.
 * For amplicons, we know exactly where reads should map!
 */
class AmpliconAssigner {
public:
    AmpliconAssigner(const std::vector<AmpliconTarget>& targets);
    ~AmpliconAssigner();

    /**
     * Assign a read cluster to an amplicon
     *
     * @param cluster Read cluster to assign
     * @return Amplicon ID or empty string if unassigned
     */
    std::string assign(const ReadCluster& cluster);

    /**
     * Batch assignment for multiple clusters
     */
    void assign_batch(std::vector<ReadCluster>& clusters);

    /**
     * Get assignment statistics
     */
    struct AssignmentStats {
        uint32_t total_clusters;
        uint32_t assigned_clusters;
        uint32_t unassigned_clusters;
        std::unordered_map<std::string, uint32_t> per_amplicon_counts;
    };

    AssignmentStats get_stats() const { return stats_; }

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;

    AssignmentStats stats_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_ASSIGNER_H
