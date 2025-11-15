#include "winalign/amplicon/amplicon_assigner.h"
#include <algorithm>
#include <unordered_map>

namespace {

// Simple substring search for primer matching
bool contains_subsequence(const std::string& text, const std::string& pattern, uint32_t max_mismatches = 2) {
    if (pattern.empty() || text.length() < pattern.length()) {
        return false;
    }

    // Sliding window search
    for (size_t i = 0; i <= text.length() - pattern.length(); ++i) {
        uint32_t mismatches = 0;

        for (size_t j = 0; j < pattern.length(); ++j) {
            if (text[i + j] != pattern[j]) {
                mismatches++;
                if (mismatches > max_mismatches) {
                    break;
                }
            }
        }

        if (mismatches <= max_mismatches) {
            return true;
        }
    }

    return false;
}

// Reverse complement
std::string reverse_complement(const std::string& seq) {
    std::string rc;
    rc.reserve(seq.length());

    for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
        switch (*it) {
            case 'A': case 'a': rc += 'T'; break;
            case 'T': case 't': rc += 'A'; break;
            case 'C': case 'c': rc += 'G'; break;
            case 'G': case 'g': rc += 'C'; break;
            case 'N': case 'n': rc += 'N'; break;
            default: rc += *it;
        }
    }

    return rc;
}

// Calculate match score between two sequences
float calculate_match_score(const std::string& seq1, const std::string& seq2) {
    if (seq1.length() != seq2.length()) {
        size_t min_len = std::min(seq1.length(), seq2.length());
        uint32_t matches = 0;

        for (size_t i = 0; i < min_len; ++i) {
            if (seq1[i] == seq2[i]) {
                matches++;
            }
        }

        return static_cast<float>(matches) / min_len;
    }

    uint32_t matches = 0;
    for (size_t i = 0; i < seq1.length(); ++i) {
        if (seq1[i] == seq2[i]) {
            matches++;
        }
    }

    return static_cast<float>(matches) / seq1.length();
}

} // anonymous namespace

namespace winalign {
namespace amplicon {

class AmpliconAssigner::Impl {
public:
    Impl(const std::vector<AmpliconTarget>& targets)
        : targets_(targets) {

        // Build primer lookup tables for faster matching
        for (const auto& target : targets_) {
            primer_to_amplicon_[target.primer_fwd] = target.amplicon_id;
            primer_to_amplicon_[target.primer_rev] = target.amplicon_id;

            // Also store reverse complement of primers
            primer_to_amplicon_[reverse_complement(target.primer_fwd)] = target.amplicon_id;
            primer_to_amplicon_[reverse_complement(target.primer_rev)] = target.amplicon_id;
        }
    }

    std::string assign(const ReadCluster& cluster) {
        // Strategy 1: Exact primer matching (fast)
        std::string amplicon_id = find_by_primer_exact(cluster.consensus_sequence);
        if (!amplicon_id.empty()) {
            return amplicon_id;
        }

        // Strategy 2: Fuzzy primer matching (allow mismatches)
        amplicon_id = find_by_primer_fuzzy(cluster.consensus_sequence);
        if (!amplicon_id.empty()) {
            return amplicon_id;
        }

        // Strategy 3: Alignment-based (fallback, slower)
        amplicon_id = find_by_alignment(cluster.consensus_sequence);
        if (!amplicon_id.empty()) {
            return amplicon_id;
        }

        // Unassigned
        stats_.unassigned_clusters++;
        return "";
    }

    void assign_batch(std::vector<ReadCluster>& clusters) {
        stats_.total_clusters = clusters.size();
        stats_.assigned_clusters = 0;
        stats_.unassigned_clusters = 0;
        stats_.per_amplicon_counts.clear();

        for (auto& cluster : clusters) {
            std::string amplicon_id = assign(cluster);
            cluster.amplicon_id = amplicon_id;

            if (!amplicon_id.empty()) {
                stats_.assigned_clusters++;
                stats_.per_amplicon_counts[amplicon_id]++;
            }
        }
    }

    AssignmentStats get_stats() const {
        return stats_;
    }

private:
    // Fast exact primer lookup
    std::string find_by_primer_exact(const std::string& sequence) {
        // Check if sequence starts with any known primer (common case)
        for (const auto& target : targets_) {
            if (sequence.length() >= target.primer_fwd.length()) {
                if (sequence.substr(0, target.primer_fwd.length()) == target.primer_fwd) {
                    return target.amplicon_id;
                }
            }

            if (sequence.length() >= target.primer_rev.length()) {
                if (sequence.substr(0, target.primer_rev.length()) == target.primer_rev) {
                    return target.amplicon_id;
                }
            }
        }

        return "";
    }

    // Fuzzy primer matching (allow 1-2 mismatches)
    std::string find_by_primer_fuzzy(const std::string& sequence) {
        float best_score = 0.0f;
        std::string best_amplicon;

        for (const auto& target : targets_) {
            // Check forward primer
            if (contains_subsequence(sequence, target.primer_fwd, 2)) {
                return target.amplicon_id;
            }

            // Check reverse primer
            if (contains_subsequence(sequence, target.primer_rev, 2)) {
                return target.amplicon_id;
            }

            // Check reverse complement
            std::string rc = reverse_complement(sequence);
            if (contains_subsequence(rc, target.primer_fwd, 2) ||
                contains_subsequence(rc, target.primer_rev, 2)) {
                return target.amplicon_id;
            }
        }

        return best_amplicon;
    }

    // Alignment-based assignment (slow fallback)
    std::string find_by_alignment(const std::string& sequence) {
        // For reads without clear primers, we would align to expected amplicon regions
        // This is a simplified version - just check length compatibility

        float best_score = 0.0f;
        std::string best_amplicon;

        for (const auto& target : targets_) {
            // Check if read length is compatible with expected amplicon length
            uint32_t expected_len = target.expected_length;
            uint32_t read_len = sequence.length();

            // Allow ±20% variation in length
            if (read_len >= expected_len * 0.8 && read_len <= expected_len * 1.2) {
                // This is a candidate - in full implementation, would do actual alignment
                // For now, just assign based on length compatibility
                float length_score = 1.0f - std::abs(static_cast<int>(read_len) -
                                                      static_cast<int>(expected_len)) /
                                             static_cast<float>(expected_len);

                if (length_score > best_score) {
                    best_score = length_score;
                    best_amplicon = target.amplicon_id;
                }
            }
        }

        // Only return if we have a reasonable score
        if (best_score > 0.8f) {
            return best_amplicon;
        }

        return "";
    }

    std::vector<AmpliconTarget> targets_;
    std::unordered_map<std::string, std::string> primer_to_amplicon_;
    AssignmentStats stats_;
};

// Public interface
AmpliconAssigner::AmpliconAssigner(const std::vector<AmpliconTarget>& targets)
    : pimpl_(std::make_unique<Impl>(targets)) {}

AmpliconAssigner::~AmpliconAssigner() = default;

std::string AmpliconAssigner::assign(const ReadCluster& cluster) {
    return pimpl_->assign(cluster);
}

void AmpliconAssigner::assign_batch(std::vector<ReadCluster>& clusters) {
    pimpl_->assign_batch(clusters);
}

AmpliconAssigner::AssignmentStats AmpliconAssigner::get_stats() const {
    return pimpl_->get_stats();
}

} // namespace amplicon
} // namespace winalign
