#include "winalign/amplicon/smith_waterman.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace winalign {
namespace amplicon {

/**
 * Implementation of Smith-Waterman aligner
 */
class SmithWatermanAligner::Impl {
public:
    SWParams params_;
    std::vector<int32_t> score_matrix_;
    std::vector<char> traceback_;
    size_t max_query_len_;
    size_t max_target_len_;

    Impl(const SWParams& params)
        : params_(params), max_query_len_(1024), max_target_len_(1024) {
        // Pre-allocate matrices for typical amplicon sizes
        score_matrix_.resize(max_query_len_ * max_target_len_);
        traceback_.resize(max_query_len_ * max_target_len_);
    }

    void resize_if_needed(size_t query_len, size_t target_len) {
        if (query_len > max_query_len_ || target_len > max_target_len_) {
            max_query_len_ = std::max(max_query_len_, query_len);
            max_target_len_ = std::max(max_target_len_, target_len);
            score_matrix_.resize(max_query_len_ * max_target_len_);
            traceback_.resize(max_query_len_ * max_target_len_);
        }
    }

    SWAlignment align(const std::string& query, const std::string& target) {
        const size_t qlen = query.length();
        const size_t tlen = target.length();

        if (qlen == 0 || tlen == 0) {
            return SWAlignment();
        }

        resize_if_needed(qlen + 1, tlen + 1);

        // Initialize matrices
        std::memset(score_matrix_.data(), 0, (qlen + 1) * (tlen + 1) * sizeof(int32_t));
        std::memset(traceback_.data(), 0, (qlen + 1) * (tlen + 1) * sizeof(char));

        // Traceback codes
        const char DIAG = 1;   // Match/mismatch
        const char UP = 2;     // Deletion in query
        const char LEFT = 3;   // Insertion in query

        int32_t max_score = 0;
        size_t max_i = 0, max_j = 0;

        // Fill score matrix
        for (size_t i = 1; i <= qlen; ++i) {
            for (size_t j = 1; j <= tlen; ++j) {
                const size_t idx = i * (tlen + 1) + j;
                const char q_base = query[i - 1];
                const char t_base = target[j - 1];

                // Match/mismatch score
                int32_t match_score = score_matrix_[(i - 1) * (tlen + 1) + (j - 1)] +
                                     (q_base == t_base ? params_.match_score : params_.mismatch_penalty);

                // Gap scores
                int32_t del_score = score_matrix_[(i - 1) * (tlen + 1) + j];
                int32_t ins_score = score_matrix_[i * (tlen + 1) + (j - 1)];

                // Apply gap penalties
                if (traceback_[(i - 1) * (tlen + 1) + j] == UP) {
                    del_score += params_.gap_extend;  // Gap extension
                } else {
                    del_score += params_.gap_open;    // Gap open
                }

                if (traceback_[i * (tlen + 1) + (j - 1)] == LEFT) {
                    ins_score += params_.gap_extend;
                } else {
                    ins_score += params_.gap_open;
                }

                // Take maximum (including 0 for local alignment)
                int32_t max_val = 0;
                char direction = 0;

                if (match_score > max_val) {
                    max_val = match_score;
                    direction = DIAG;
                }
                if (del_score > max_val) {
                    max_val = del_score;
                    direction = UP;
                }
                if (ins_score > max_val) {
                    max_val = ins_score;
                    direction = LEFT;
                }

                score_matrix_[idx] = max_val;
                traceback_[idx] = direction;

                // Track maximum score for local alignment
                if (max_val > max_score) {
                    max_score = max_val;
                    max_i = i;
                    max_j = j;
                }
            }
        }

        // Traceback from maximum score
        SWAlignment result;
        result.score = max_score;
        result.ref_end = max_j;
        result.query_end = max_i;

        if (max_score == 0) {
            return result;  // No alignment
        }

        // Perform traceback
        std::vector<char> cigar_ops;
        size_t i = max_i;
        size_t j = max_j;
        uint32_t matches = 0;
        uint32_t mismatches = 0;
        uint32_t gaps = 0;

        while (i > 0 && j > 0) {
            const size_t idx = i * (tlen + 1) + j;
            const char dir = traceback_[idx];

            if (dir == 0 || score_matrix_[idx] == 0) {
                break;  // Reached start of local alignment
            }

            if (dir == DIAG) {
                if (query[i - 1] == target[j - 1]) {
                    cigar_ops.push_back('M');  // Match
                    matches++;
                } else {
                    cigar_ops.push_back('X');  // Mismatch
                    mismatches++;
                }
                i--;
                j--;
            } else if (dir == UP) {
                cigar_ops.push_back('D');  // Deletion
                gaps++;
                i--;
            } else if (dir == LEFT) {
                cigar_ops.push_back('I');  // Insertion
                gaps++;
                j--;
            }
        }

        result.ref_start = j;
        result.query_start = i;
        result.matches = matches;
        result.mismatches = mismatches;
        result.gaps = gaps;
        result.identity = calculate_identity(matches, mismatches, gaps);

        // Compress CIGAR string
        std::reverse(cigar_ops.begin(), cigar_ops.end());
        result.cigar = compress_cigar(cigar_ops);

        return result;
    }

private:
    std::string compress_cigar(const std::vector<char>& ops) {
        if (ops.empty()) return "";

        std::string cigar;
        char last_op = ops[0];
        uint32_t count = 1;

        for (size_t i = 1; i < ops.size(); ++i) {
            if (ops[i] == last_op) {
                count++;
            } else {
                cigar += std::to_string(count) + last_op;
                last_op = ops[i];
                count = 1;
            }
        }
        cigar += std::to_string(count) + last_op;

        return cigar;
    }
};

// Public interface implementation

SmithWatermanAligner::SmithWatermanAligner(const SWParams& params)
    : pimpl_(std::make_unique<Impl>(params)) {}

SmithWatermanAligner::~SmithWatermanAligner() = default;

SWAlignment SmithWatermanAligner::align(const std::string& query, const std::string& target) {
    return pimpl_->align(query, target);
}

std::pair<SWAlignment, int> SmithWatermanAligner::align_to_best_target(
    const std::string& query,
    const std::vector<AmpliconTarget>& targets,
    const std::vector<std::string>& target_sequences
) {
    SWAlignment best_alignment;
    int best_idx = -1;
    int32_t best_score = 0;

    for (size_t i = 0; i < targets.size() && i < target_sequences.size(); ++i) {
        SWAlignment aln = pimpl_->align(query, target_sequences[i]);
        if (aln.score > best_score) {
            best_score = aln.score;
            best_alignment = aln;
            best_idx = static_cast<int>(i);
        }
    }

    return {best_alignment, best_idx};
}

void SmithWatermanAligner::set_params(const SWParams& params) {
    pimpl_->params_ = params;
}

SWParams SmithWatermanAligner::get_params() const {
    return pimpl_->params_;
}

// Utility functions

std::string generate_cigar(
    const std::string& query,
    const std::string& target,
    const std::vector<char>& traceback
) {
    // Simple implementation - traceback already contains operations
    std::string cigar;
    if (traceback.empty()) return "0M";

    char last_op = traceback[0];
    uint32_t count = 1;

    for (size_t i = 1; i < traceback.size(); ++i) {
        if (traceback[i] == last_op) {
            count++;
        } else {
            cigar += std::to_string(count) + last_op;
            last_op = traceback[i];
            count = 1;
        }
    }
    cigar += std::to_string(count) + last_op;

    return cigar;
}

float calculate_identity(uint32_t matches, uint32_t mismatches, uint32_t gaps) {
    uint32_t total = matches + mismatches + gaps;
    if (total == 0) return 0.0f;
    return (static_cast<float>(matches) / static_cast<float>(total)) * 100.0f;
}

} // namespace amplicon
} // namespace winalign
