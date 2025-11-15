#ifndef WINALIGN_AMPLICON_REALTIME_PROCESSOR_H
#define WINALIGN_AMPLICON_REALTIME_PROCESSOR_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>

namespace winalign {
namespace amplicon {

/**
 * Real-time processing configuration
 */
struct RealtimeConfig {
    std::string watch_directory;     // Directory to monitor for FASTQ files
    std::string output_directory;    // Where to write results
    uint32_t poll_interval_ms;       // How often to check for new files (ms)
    uint32_t batch_size;             // Reads to accumulate before processing
    uint32_t max_queue_size;         // Max reads in processing queue
    bool process_incremental;        // Process files as they grow
    bool emit_partial_results;       // Emit results before file complete

    RealtimeConfig()
        : poll_interval_ms(1000), batch_size(10000),
          max_queue_size(100000), process_incremental(true),
          emit_partial_results(false) {}
};

/**
 * File event types
 */
enum class FileEvent {
    CREATED,    // New file appeared
    MODIFIED,   // File was written to
    COMPLETED,  // File finished writing
    DELETED     // File was removed
};

/**
 * Callback for file events
 */
using FileEventCallback = std::function<void(const std::string&, FileEvent)>;

/**
 * Callback for processing progress
 */
using ProgressCallback = std::function<void(uint64_t reads_processed, float reads_per_sec)>;

/**
 * Callback for variant discovery
 */
using VariantCallback = std::function<void(const AmpliconVariant&)>;

/**
 * Real-time amplicon processor
 *
 * Monitors a directory for incoming FASTQ files and processes them
 * as data arrives from the sequencer. Useful for:
 * - Live sequencing runs
 * - Streaming analysis
 * - Early termination based on coverage
 */
class RealtimeProcessor {
public:
    RealtimeProcessor(
        const RealtimeConfig& config,
        const AmpliconConfig& amplicon_config
    );
    ~RealtimeProcessor();

    /**
     * Start real-time processing
     * Runs in background thread
     */
    void start();

    /**
     * Stop processing and wait for completion
     */
    void stop();

    /**
     * Check if processor is running
     */
    bool is_running() const;

    /**
     * Set callbacks
     */
    void set_file_event_callback(FileEventCallback callback);
    void set_progress_callback(ProgressCallback callback);
    void set_variant_callback(VariantCallback callback);

    /**
     * Get current statistics
     */
    struct Stats {
        uint64_t files_processed;
        uint64_t reads_processed;
        uint64_t variants_called;
        uint64_t total_bases;
        float current_throughput;  // reads/sec
        std::chrono::system_clock::time_point start_time;
        std::chrono::system_clock::time_point last_update;
    };

    Stats get_stats() const;

    /**
     * Get coverage statistics (for early termination)
     */
    struct CoverageStats {
        std::unordered_map<std::string, uint32_t> amplicon_coverage;
        uint32_t min_coverage;
        uint32_t max_coverage;
        float mean_coverage;
        bool sufficient_coverage;  // All amplicons above threshold
    };

    CoverageStats get_coverage_stats() const;

    /**
     * Check if sufficient coverage achieved
     * Can be used to stop sequencing early
     */
    bool has_sufficient_coverage(uint32_t min_depth = 1000) const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Directory watcher for monitoring FASTQ files
 */
class DirectoryWatcher {
public:
    DirectoryWatcher(const std::string& directory);
    ~DirectoryWatcher();

    /**
     * Start watching directory
     */
    void start(FileEventCallback callback);

    /**
     * Stop watching
     */
    void stop();

    /**
     * Check for new/modified files (polling mode)
     */
    std::vector<std::string> check_for_changes();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Incremental FASTQ reader
 * Reads FASTQ file as it's being written
 */
class IncrementalFASTQReader {
public:
    IncrementalFASTQReader(const std::string& filename);
    ~IncrementalFASTQReader();

    /**
     * Read next batch of reads
     * Returns empty vector if no new data
     */
    std::vector<Read> read_next_batch(size_t batch_size);

    /**
     * Check if file has more data
     */
    bool has_more_data() const;

    /**
     * Check if file is complete (no longer being written)
     */
    bool is_complete() const;

    /**
     * Reset to beginning of file
     */
    void reset();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_REALTIME_PROCESSOR_H
