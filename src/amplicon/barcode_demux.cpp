#include "winalign/amplicon/barcode_demux.h"
#include "winalign/amplicon/primer_trimmer.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>

namespace winalign {
namespace amplicon {

/**
 * Implementation of barcode demultiplexer
 */
class BarcodeDemultiplexer::Impl {
public:
    DemuxParams params_;
    std::vector<SampleBarcode> barcodes_;
    std::vector<SampleBarcode> i5_barcodes_;
    std::vector<SampleBarcode> i7_barcodes_;
    Stats stats_;

    Impl(const DemuxParams& params)
        : params_(params) {
        reset_stats();
    }

    void reset_stats() {
        stats_ = Stats{0, 0, 0, 0, 0, {}, 0.0f};
    }

    DemuxResult demultiplex(const std::string& sequence, const std::string& quality) {
        DemuxResult result;
        stats_.total_reads++;

        if (barcodes_.empty()) {
            return result;  // No barcodes configured
        }

        // Try to find matching barcode
        int best_barcode_idx = -1;
        uint32_t best_mismatches = params_.max_mismatches + 1;
        int32_t best_position = -1;
        std::vector<int> equally_good_matches;

        for (size_t i = 0; i < barcodes_.size(); ++i) {
            const auto& bc = barcodes_[i];

            // Search at expected position (with some tolerance)
            uint32_t search_region = std::min(50u, static_cast<uint32_t>(sequence.length()));
            auto [pos, mismatches] = find_barcode(sequence, bc.barcode,
                                                   params_.max_mismatches, search_region);

            if (pos >= 0 && mismatches < best_mismatches) {
                best_mismatches = mismatches;
                best_barcode_idx = i;
                best_position = pos;
                equally_good_matches.clear();
                equally_good_matches.push_back(i);
            } else if (pos >= 0 && mismatches == best_mismatches) {
                equally_good_matches.push_back(i);
            }

            // Check reverse complement if enabled
            if (params_.check_reverse_complement && !bc.barcode_rc.empty()) {
                auto [pos_rc, mm_rc] = find_barcode(sequence, bc.barcode_rc,
                                                     params_.max_mismatches, search_region);

                if (pos_rc >= 0 && mm_rc < best_mismatches) {
                    best_mismatches = mm_rc;
                    best_barcode_idx = i;
                    best_position = pos_rc;
                    equally_good_matches.clear();
                    equally_good_matches.push_back(i);
                } else if (pos_rc >= 0 && mm_rc == best_mismatches) {
                    equally_good_matches.push_back(i);
                }
            }
        }

        // Check if assignment is ambiguous
        if (equally_good_matches.size() > 1) {
            result.ambiguous = true;
            stats_.ambiguous++;
            return result;
        }

        // Check if barcode was found
        if (best_barcode_idx < 0) {
            stats_.unassigned++;
            return result;
        }

        const auto& matched_bc = barcodes_[best_barcode_idx];
        result.sample_id = matched_bc.sample_id;
        result.barcode_found = matched_bc.barcode;
        result.mismatches = best_mismatches;

        // Check quality in barcode region
        if (!quality.empty() && quality.length() == sequence.length()) {
            float avg_qual = average_quality(quality, best_position,
                                             matched_bc.barcode.length());
            if (avg_qual < params_.min_quality) {
                result.passed_qc = false;
                stats_.qc_failed++;
                return result;
            }
        }

        result.passed_qc = true;
        stats_.assigned++;
        stats_.per_sample_counts[result.sample_id]++;

        // Update assignment rate
        if (stats_.total_reads > 0) {
            stats_.assignment_rate = static_cast<float>(stats_.assigned) /
                                    static_cast<float>(stats_.total_reads);
        }

        // Trim barcode if requested
        if (params_.trim_barcodes && best_position >= 0) {
            result.trim_length = best_position + matched_bc.barcode.length();
        }

        return result;
    }

    DemuxResult demultiplex_paired(
        const std::string& seq1, const std::string& qual1,
        const std::string& seq2, const std::string& qual2
    ) {
        // For paired-end, typically i7 is in R1 and i5 is in R2
        if (params_.dual_index && !i5_barcodes_.empty() && !i7_barcodes_.empty()) {
            return demultiplex_dual_index(seq1, qual1, seq2, qual2);
        }

        // Single index - check both reads
        DemuxResult r1 = demultiplex(seq1, qual1);
        if (r1.assigned()) return r1;

        return demultiplex(seq2, qual2);
    }

private:
    DemuxResult demultiplex_dual_index(
        const std::string& seq1, const std::string& qual1,
        const std::string& seq2, const std::string& qual2
    ) {
        DemuxResult result;

        // Find i7 barcode in R1
        int best_i7 = -1;
        uint32_t best_i7_mm = params_.max_mismatches + 1;

        for (size_t i = 0; i < i7_barcodes_.size(); ++i) {
            auto [pos, mm] = find_barcode(seq1, i7_barcodes_[i].barcode,
                                         params_.max_mismatches, 50);
            if (pos >= 0 && mm < best_i7_mm) {
                best_i7 = i;
                best_i7_mm = mm;
            }
        }

        // Find i5 barcode in R2
        int best_i5 = -1;
        uint32_t best_i5_mm = params_.max_mismatches + 1;

        for (size_t i = 0; i < i5_barcodes_.size(); ++i) {
            auto [pos, mm] = find_barcode(seq2, i5_barcodes_[i].barcode,
                                         params_.max_mismatches, 50);
            if (pos >= 0 && mm < best_i5_mm) {
                best_i5 = i;
                best_i5_mm = mm;
            }
        }

        // Both barcodes must match for dual indexing
        if (best_i7 >= 0 && best_i5 >= 0) {
            // Check if they correspond to the same sample
            const std::string& i7_sample = i7_barcodes_[best_i7].sample_id;
            const std::string& i5_sample = i5_barcodes_[best_i5].sample_id;

            if (i7_sample == i5_sample) {
                result.sample_id = i7_sample;
                result.passed_qc = true;
                result.mismatches = best_i7_mm + best_i5_mm;
                stats_.assigned++;
                stats_.per_sample_counts[result.sample_id]++;
            } else {
                result.ambiguous = true;
                stats_.ambiguous++;
            }
        } else {
            stats_.unassigned++;
        }

        return result;
    }
};

// Public interface

BarcodeDemultiplexer::BarcodeDemultiplexer(const DemuxParams& params)
    : pimpl_(std::make_unique<Impl>(params)) {}

BarcodeDemultiplexer::~BarcodeDemultiplexer() = default;

void BarcodeDemultiplexer::set_barcodes(const std::vector<SampleBarcode>& barcodes) {
    pimpl_->barcodes_ = barcodes;

    // Pre-compute reverse complements
    for (auto& bc : pimpl_->barcodes_) {
        bc.barcode_rc = reverse_complement(bc.barcode);
    }
}

void BarcodeDemultiplexer::set_dual_barcodes(
    const std::vector<SampleBarcode>& i5_barcodes,
    const std::vector<SampleBarcode>& i7_barcodes
) {
    pimpl_->i5_barcodes_ = i5_barcodes;
    pimpl_->i7_barcodes_ = i7_barcodes;
    pimpl_->params_.dual_index = true;
}

DemuxResult BarcodeDemultiplexer::demultiplex(
    const std::string& sequence,
    const std::string& quality
) {
    return pimpl_->demultiplex(sequence, quality);
}

DemuxResult BarcodeDemultiplexer::demultiplex_paired(
    const std::string& seq1,
    const std::string& qual1,
    const std::string& seq2,
    const std::string& qual2
) {
    return pimpl_->demultiplex_paired(seq1, qual1, seq2, qual2);
}

DemuxResult BarcodeDemultiplexer::demultiplex_cluster(ReadCluster& cluster) {
    auto result = pimpl_->demultiplex(cluster.consensus_sequence,
                                      cluster.consensus_quality);

    // Trim barcode if needed
    if (result.assigned() && result.trim_length > 0) {
        cluster.consensus_sequence = cluster.consensus_sequence.substr(result.trim_length);
        if (!cluster.consensus_quality.empty()) {
            cluster.consensus_quality = cluster.consensus_quality.substr(result.trim_length);
        }
    }

    return result;
}

std::unordered_map<std::string, std::vector<ReadCluster>>
BarcodeDemultiplexer::demultiplex_batch(const std::vector<ReadCluster>& clusters) {
    std::unordered_map<std::string, std::vector<ReadCluster>> sample_map;

    for (const auto& cluster : clusters) {
        ReadCluster working_cluster = cluster;
        DemuxResult result = demultiplex_cluster(working_cluster);

        if (result.assigned()) {
            sample_map[result.sample_id].push_back(working_cluster);
        } else {
            // Unassigned reads go to "unassigned" bin
            sample_map["unassigned"].push_back(cluster);
        }
    }

    return sample_map;
}

BarcodeDemultiplexer::Stats BarcodeDemultiplexer::get_stats() const {
    return pimpl_->stats_;
}

void BarcodeDemultiplexer::reset_stats() {
    pimpl_->reset_stats();
}

// Utility functions

std::pair<int32_t, uint32_t> find_barcode(
    const std::string& sequence,
    const std::string& barcode,
    uint32_t max_mismatches,
    uint32_t search_region
) {
    if (barcode.empty() || sequence.empty()) {
        return {-1, max_mismatches + 1};
    }

    search_region = std::min(search_region, static_cast<uint32_t>(sequence.length()));
    int32_t best_pos = -1;
    uint32_t best_mismatches = max_mismatches + 1;

    // Search in the first search_region bases
    for (uint32_t offset = 0; offset <= search_region - barcode.length() &&
                               offset < sequence.length(); ++offset) {
        uint32_t mismatches = 0;

        for (size_t i = 0; i < barcode.length() && (offset + i) < sequence.length(); ++i) {
            char s = std::toupper(sequence[offset + i]);
            char b = std::toupper(barcode[i]);

            if (s != b && s != 'N' && b != 'N') {
                mismatches++;
                if (mismatches > max_mismatches) break;
            }
        }

        if (mismatches < best_mismatches) {
            best_pos = offset;
            best_mismatches = mismatches;

            // Exact match - stop searching
            if (mismatches == 0) break;
        }
    }

    return {best_pos, best_mismatches};
}

float average_quality(
    const std::string& quality,
    size_t start,
    size_t length
) {
    if (quality.empty() || start >= quality.length()) {
        return 0.0f;
    }

    size_t end = std::min(start + length, quality.length());
    float sum = 0.0f;
    size_t count = 0;

    for (size_t i = start; i < end; ++i) {
        // Convert PHRED+33 to quality score
        sum += static_cast<float>(quality[i] - 33);
        count++;
    }

    return count > 0 ? (sum / count) : 0.0f;
}

std::vector<SampleBarcode> load_barcodes_from_file(const std::string& filename) {
    std::vector<SampleBarcode> barcodes;
    std::ifstream file(filename);

    if (!file.is_open()) {
        return barcodes;
    }

    std::string line;
    // Skip header if present
    if (std::getline(file, line) && line.find("sample_id") != std::string::npos) {
        // Header line, skip it
    } else {
        // No header, process this line
        file.seekg(0);
    }

    while (std::getline(file, line)) {
        std::istringstream iss(line);
        std::string sample_id, barcode, position_str;

        if (std::getline(iss, sample_id, ',') &&
            std::getline(iss, barcode, ',')) {

            SampleBarcode bc;
            bc.sample_id = sample_id;
            bc.barcode = barcode;

            if (std::getline(iss, position_str, ',')) {
                bc.expected_position = std::stoul(position_str);
            } else {
                bc.expected_position = 0;  // Default to 5' end
            }

            bc.barcode_rc = reverse_complement(bc.barcode);
            barcodes.push_back(bc);
        }
    }

    return barcodes;
}

} // namespace amplicon
} // namespace winalign
