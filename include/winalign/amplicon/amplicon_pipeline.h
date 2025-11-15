#ifndef WINALIGN_AMPLICON_PIPELINE_H
#define WINALIGN_AMPLICON_PIPELINE_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <functional>
#include <memory>

namespace winalign {
namespace amplicon {

/**
 * Amplicon Pipeline
 *
 * Main orchestrator for amplicon sequencing data processing.
 * Separate from WGS pipeline due to different optimization strategies.
 *
 * Pipeline:
 *   1. Read ingestion
 *   2. Read collapsing (10,000 -> ~50 unique)
 *   3. Amplicon assignment (no k-mer seeding!)
 *   4. GPU alignment (only unique sequences)
 *   5. Variant calling with depth tracking
 *   6. VCF output
 */
class AmpliconPipeline {
public:
    explicit AmpliconPipeline(const AmpliconConfig& config);
    ~AmpliconPipeline();

    // Disable copy
    AmpliconPipeline(const AmpliconPipeline&) = delete;
    AmpliconPipeline& operator=(const AmpliconPipeline&) = delete;

    /**
     * Initialize pipeline
     * - Load reference genome
     * - Load amplicon targets
     * - Initialize GPU
     */
    Result<bool> initialize();

    /**
     * Run pipeline on input FASTQ
     */
    Result<bool> run();

    /**
     * Finalize and cleanup
     */
    Result<bool> finalize();

    /**
     * Get overall statistics
     */
    struct PipelineStats {
        uint32_t total_reads;
        uint32_t collapsed_clusters;
        uint32_t assigned_clusters;
        uint32_t aligned_clusters;
        uint32_t variants_called;
        float collapse_ratio;      // total_reads / collapsed_clusters
        float assignment_rate;     // assigned / collapsed
        float alignment_rate;      // aligned / assigned
    };

    PipelineStats get_stats() const;

    /**
     * Set progress callback (0.0 to 1.0)
     */
    void set_progress_callback(std::function<void(double, const std::string&)> callback);

    /**
     * Cancel processing
     */
    void cancel();

    /**
     * Check if running
     */
    bool is_running() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_PIPELINE_H
