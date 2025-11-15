#ifndef WINALIGN_AMPLICON_BARCODE_DEMUX_H
#define WINALIGN_AMPLICON_BARCODE_DEMUX_H

#include "amplicon_types.h"
#include "winalign/common.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace winalign {
namespace amplicon {

/**
 * Sample barcode definition
 */
struct SampleBarcode {
    std::string sample_id;          // Sample identifier
    std::string barcode;            // Barcode sequence (5' or 3')
    std::string barcode_rc;         // Reverse complement
    uint32_t expected_position;     // Expected position in read (0=5', -1=3')

    SampleBarcode() : expected_position(0) {}
};

/**
 * Barcode demultiplexing parameters
 */
struct DemuxParams {
    uint32_t max_mismatches;        // Max mismatches allowed (default: 1)
    uint32_t min_quality;           // Min average quality in barcode region
    bool check_reverse_complement;  // Check RC of barcodes
    bool trim_barcodes;             // Remove barcodes after assignment
    bool dual_index;                // Use dual indexing (i5 + i7)

    DemuxParams()
        : max_mismatches(1), min_quality(20),
          check_reverse_complement(true), trim_barcodes(true),
          dual_index(false) {}
};

/**
 * Demultiplexing result
 */
struct DemuxResult {
    std::string sample_id;          // Assigned sample ID
    std::string barcode_found;      // Barcode sequence found
    uint32_t mismatches;            // Number of mismatches
    bool passed_qc;                 // Passed quality filter
    bool ambiguous;                 // Multiple barcodes matched equally
    uint32_t trim_length;           // Bases trimmed

    DemuxResult()
        : mismatches(0), passed_qc(false), ambiguous(false), trim_length(0) {}

    bool assigned() const { return !sample_id.empty() && passed_qc; }
};

/**
 * Barcode demultiplexer for multi-sample amplicon data
 *
 * Separates reads by sample barcodes for joint analysis.
 * Supports single and dual indexing strategies.
 */
class BarcodeDemultiplexer {
public:
    BarcodeDemultiplexer(const DemuxParams& params = DemuxParams());
    ~BarcodeDemultiplexer();

    /**
     * Set sample barcodes for demultiplexing
     */
    void set_barcodes(const std::vector<SampleBarcode>& barcodes);

    /**
     * Set dual index barcodes (i5 and i7)
     */
    void set_dual_barcodes(
        const std::vector<SampleBarcode>& i5_barcodes,
        const std::vector<SampleBarcode>& i7_barcodes
    );

    /**
     * Demultiplex a single read
     *
     * @param sequence Read sequence
     * @param quality Quality scores
     * @return Demultiplexing result
     */
    DemuxResult demultiplex(
        const std::string& sequence,
        const std::string& quality
    );

    /**
     * Demultiplex read pair (paired-end sequencing)
     */
    DemuxResult demultiplex_paired(
        const std::string& seq1,
        const std::string& qual1,
        const std::string& seq2,
        const std::string& qual2
    );

    /**
     * Demultiplex read cluster
     */
    DemuxResult demultiplex_cluster(ReadCluster& cluster);

    /**
     * Batch demultiplexing - assigns reads to samples
     * Returns map of sample_id -> reads
     */
    std::unordered_map<std::string, std::vector<ReadCluster>>
    demultiplex_batch(const std::vector<ReadCluster>& clusters);

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t total_reads;
        uint64_t assigned;
        uint64_t unassigned;
        uint64_t ambiguous;
        uint64_t qc_failed;
        std::unordered_map<std::string, uint64_t> per_sample_counts;
        float assignment_rate;
    };

    Stats get_stats() const;
    void reset_stats();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Find barcode in sequence with mismatches allowed
 * Returns position and number of mismatches, or -1 if not found
 */
std::pair<int32_t, uint32_t> find_barcode(
    const std::string& sequence,
    const std::string& barcode,
    uint32_t max_mismatches,
    uint32_t search_region = 50
);

/**
 * Calculate average quality in a region
 */
float average_quality(
    const std::string& quality,
    size_t start,
    size_t length
);

/**
 * Load barcodes from CSV file
 * Format: sample_id,barcode,position
 */
std::vector<SampleBarcode> load_barcodes_from_file(
    const std::string& filename
);

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_BARCODE_DEMUX_H
