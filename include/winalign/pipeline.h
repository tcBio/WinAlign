#ifndef WINALIGN_PIPELINE_H
#define WINALIGN_PIPELINE_H

#include "common.h"
#include <memory>
#include <map>
#include <functional>

namespace winalign {

/**
 * @brief Pipeline configuration
 */
struct PipelineConfig {
    // Input files
    std::string reference_fasta;
    std::string read1_fastq;
    std::string read2_fastq;  // Empty for single-end

    // Output files
    std::string output_bam;
    std::string output_metrics;

    // Processing parameters
    size_t batch_size = DEFAULT_BATCH_SIZE;
    size_t kmer_size = DEFAULT_KMER_SIZE;
    int cpu_threads = 4;
    int gpu_device_id = 0;

    // Alignment parameters
    AlignmentScores scores;
    int min_mapping_quality = 0;
    int max_insert_size = 1000;
    int min_insert_size = 0;

    // Flags
    bool mark_duplicates = true;
    bool sort_output = true;
    bool create_index = true;
    bool use_gpu = true;
    // Fast behavior is the default: avoid expensive CPU fallbacks when GPU
    // alignment fails. A future "slow/accurate" mode could re-enable them.
    bool fast_mode = true;
    std::string log_file;

    // Validation
    bool is_valid() const;
};

/**
 * @brief QC metrics
 */
struct QCMetrics {
    uint64_t total_reads = 0;
    uint64_t aligned_reads = 0;
    uint64_t properly_paired = 0;
    uint64_t duplicates = 0;

    double mean_quality = 0.0;
    double mean_coverage = 0.0;
    double mean_insert_size = 0.0;

    std::map<std::string, uint64_t> reads_per_chromosome;

    double alignment_rate() const {
        return total_reads > 0 ? (double)aligned_reads / total_reads : 0.0;
    }
};

/**
 * @brief Main alignment pipeline
 *
 * Orchestrates the entire alignment workflow from FASTQ to BAM.
 */
class Pipeline {
public:
    /**
     * @brief Construct a new Pipeline
     * @param config Pipeline configuration
     */
    explicit Pipeline(const PipelineConfig& config);

    /**
     * @brief Destroy the Pipeline
     */
    ~Pipeline();

    // Disable copy
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    /**
     * @brief Initialize pipeline
     *
     * Loads reference, builds indices, allocates GPU memory
     * @return Result with error code
     */
    Result<bool> initialize();

    /**
     * @brief Run the alignment pipeline
     * @return Result with error code
     */
    Result<bool> run();

    /**
     * @brief Finalize pipeline
     *
     * Writes final output, generates metrics, cleans up resources
     * @return Result with error code
     */
    Result<bool> finalize();

    /**
     * @brief Get QC metrics
     * @return QC metrics
     */
    const QCMetrics& get_metrics() const;

    /**
     * @brief Set progress callback
     * @param callback Function to call with progress updates (0.0 to 1.0)
     */
    void set_progress_callback(std::function<void(double)> callback);

    /**
     * @brief Cancel running pipeline
     */
    void cancel();

    /**
     * @brief Check if pipeline is running
     * @return true if running, false otherwise
     */
    bool is_running() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace winalign

#endif // WINALIGN_PIPELINE_H
