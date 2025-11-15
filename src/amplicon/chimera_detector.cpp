#include "winalign/amplicon/chimera_detector.h"
#include <algorithm>
#include <cmath>

namespace winalign {
namespace amplicon {

/**
 * Calculate hamming distance between two sequences
 */
static uint32_t hamming_distance(const std::string& a, const std::string& b) {
    size_t len = std::min(a.length(), b.length());
    uint32_t dist = 0;
    for (size_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) dist++;
    }
    return dist + std::abs(static_cast<int>(a.length()) - static_cast<int>(b.length()));
}

/**
 * Implementation of chimera detector
 */
class ChimeraDetector::Impl {
public:
    ChimeraParams params_;
    std::vector<std::string> references_;
    std::vector<uint32_t> ref_abundances_;
    Stats stats_;

    Impl(const ChimeraParams& params)
        : params_(params) {
        reset_stats();
    }

    void reset_stats() {
        stats_ = Stats{0, 0, 0, 0, 0.0f};
    }

    ChimeraResult detect(const std::string& sequence, uint32_t abundance) {
        ChimeraResult result;
        stats_.total_checked++;

        if (references_.empty()) {
            return result;  // No references for detection
        }

        // Try de novo detection first (more sensitive)
        if (params_.use_de_novo) {
            uint32_t breakpoint = 0;
            bool is_chimera = detect_chimera_de_novo(
                sequence, references_, ref_abundances_,
                abundance, params_, breakpoint
            );

            if (is_chimera) {
                result.is_chimera = true;
                result.breakpoint = breakpoint;
                result.method = "de_novo";
                result.confidence = 0.95f;  // High confidence for de novo
                stats_.chimeras_found++;
                stats_.de_novo_detected++;
                return result;
            }
        }

        // Try reference-based detection
        if (params_.use_reference) {
            uint32_t breakpoint = 0;
            bool is_chimera = detect_chimera_reference(
                sequence, references_, params_, breakpoint
            );

            if (is_chimera) {
                result.is_chimera = true;
                result.breakpoint = breakpoint;
                result.method = "reference";
                result.confidence = 0.90f;
                stats_.chimeras_found++;
                stats_.reference_detected++;
                return result;
            }
        }

        // Update stats
        if (stats_.total_checked > 0) {
            stats_.chimera_rate = static_cast<float>(stats_.chimeras_found) /
                                 static_cast<float>(stats_.total_checked);
        }

        return result;
    }
};

// Public interface

ChimeraDetector::ChimeraDetector(const ChimeraParams& params)
    : pimpl_(std::make_unique<Impl>(params)) {}

ChimeraDetector::~ChimeraDetector() = default;

void ChimeraDetector::set_references(const std::vector<std::string>& references) {
    pimpl_->references_ = references;
    pimpl_->ref_abundances_.resize(references.size(), 1000);  // Assume high abundance
}

void ChimeraDetector::add_de_novo_references(const std::vector<ReadCluster>& clusters) {
    // Use high-abundance clusters as references
    std::vector<ReadCluster> sorted_clusters = clusters;
    std::sort(sorted_clusters.begin(), sorted_clusters.end(),
              [](const ReadCluster& a, const ReadCluster& b) {
                  return a.read_count > b.read_count;
              });

    // Take top clusters as references
    const size_t max_refs = 100;
    for (size_t i = 0; i < std::min(sorted_clusters.size(), max_refs); ++i) {
        pimpl_->references_.push_back(sorted_clusters[i].consensus_sequence);
        pimpl_->ref_abundances_.push_back(sorted_clusters[i].read_count);
    }
}

ChimeraResult ChimeraDetector::detect(const std::string& sequence, uint32_t abundance) {
    return pimpl_->detect(sequence, abundance);
}

ChimeraResult ChimeraDetector::detect_cluster(const ReadCluster& cluster) {
    return pimpl_->detect(cluster.consensus_sequence, cluster.read_count);
}

std::vector<ReadCluster> ChimeraDetector::filter_chimeras(
    const std::vector<ReadCluster>& clusters
) {
    std::vector<ReadCluster> filtered;
    filtered.reserve(clusters.size());

    for (const auto& cluster : clusters) {
        ChimeraResult result = detect_cluster(cluster);
        if (!result.is_chimera) {
            filtered.push_back(cluster);
        }
    }

    return filtered;
}

ChimeraDetector::Stats ChimeraDetector::get_stats() const {
    return pimpl_->stats_;
}

void ChimeraDetector::reset_stats() {
    pimpl_->reset_stats();
}

// Detection algorithms

bool detect_chimera_de_novo(
    const std::string& query,
    const std::vector<std::string>& references,
    const std::vector<uint32_t>& abundances,
    uint32_t query_abundance,
    const ChimeraParams& params,
    uint32_t& breakpoint
) {
    if (query.length() < params.min_segment_length * 2) {
        return false;  // Too short for chimera detection
    }

    // Find potential parent sequences (more abundant than query)
    std::vector<size_t> parent_indices;
    for (size_t i = 0; i < references.size(); ++i) {
        if (abundances[i] >= query_abundance * params.min_abundance_ratio) {
            parent_indices.push_back(i);
        }
    }

    if (parent_indices.size() < 2) {
        return false;  // Need at least 2 potential parents
    }

    // Try different breakpoint positions
    const uint32_t min_pos = params.min_segment_length;
    const uint32_t max_pos = query.length() - params.min_segment_length;

    uint32_t best_score = 0;
    uint32_t best_breakpoint = 0;

    for (uint32_t pos = min_pos; pos < max_pos; pos += 5) {  // Check every 5bp
        std::string left_segment = query.substr(0, pos);
        std::string right_segment = query.substr(pos);

        // Find best matching parents for each segment
        uint32_t best_left_dist = UINT32_MAX;
        uint32_t best_right_dist = UINT32_MAX;

        for (size_t parent_idx : parent_indices) {
            const std::string& parent = references[parent_idx];
            if (parent.length() < query.length()) continue;

            std::string parent_left = parent.substr(0, pos);
            std::string parent_right = parent.substr(pos, right_segment.length());

            uint32_t left_dist = hamming_distance(left_segment, parent_left);
            uint32_t right_dist = hamming_distance(right_segment, parent_right);

            best_left_dist = std::min(best_left_dist, left_dist);
            best_right_dist = std::min(best_right_dist, right_dist);
        }

        // Score: low distance to parents = high score
        uint32_t score = 1000 - (best_left_dist + best_right_dist);
        if (score > best_score) {
            best_score = score;
            best_breakpoint = pos;
        }
    }

    // Check if this looks like a chimera
    // Chimeras have low divergence to parents on both sides
    breakpoint = best_breakpoint;
    return best_score > (1000 - params.min_score_diff);
}

bool detect_chimera_reference(
    const std::string& query,
    const std::vector<std::string>& references,
    const ChimeraParams& params,
    uint32_t& breakpoint
) {
    if (query.length() < params.min_segment_length * 2 || references.size() < 2) {
        return false;
    }

    // Similar to de novo but uses known reference sequences
    const uint32_t min_pos = params.min_segment_length;
    const uint32_t max_pos = query.length() - params.min_segment_length;

    uint32_t best_score = 0;
    uint32_t best_breakpoint = 0;

    for (uint32_t pos = min_pos; pos < max_pos; pos += 5) {
        std::string left_segment = query.substr(0, pos);
        std::string right_segment = query.substr(pos);

        // Check all pairs of references
        for (size_t i = 0; i < references.size(); ++i) {
            for (size_t j = i + 1; j < references.size(); ++j) {
                const std::string& ref1 = references[i];
                const std::string& ref2 = references[j];

                if (ref1.length() < pos || ref2.length() < pos + right_segment.length()) {
                    continue;
                }

                // Try ref1-left + ref2-right
                std::string ref1_left = ref1.substr(0, pos);
                std::string ref2_right = ref2.substr(pos, right_segment.length());

                uint32_t dist = hamming_distance(left_segment, ref1_left) +
                               hamming_distance(right_segment, ref2_right);

                uint32_t score = 1000 - dist;
                if (score > best_score) {
                    best_score = score;
                    best_breakpoint = pos;
                }

                // Try ref2-left + ref1-right
                if (ref2.length() >= pos && ref1.length() >= pos + right_segment.length()) {
                    std::string ref2_left = ref2.substr(0, pos);
                    std::string ref1_right = ref1.substr(pos, right_segment.length());

                    dist = hamming_distance(left_segment, ref2_left) +
                          hamming_distance(right_segment, ref1_right);

                    score = 1000 - dist;
                    if (score > best_score) {
                        best_score = score;
                        best_breakpoint = pos;
                    }
                }
            }
        }
    }

    breakpoint = best_breakpoint;
    return best_score > (1000 - params.min_score_diff);
}

} // namespace amplicon
} // namespace winalign
