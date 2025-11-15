#include "winalign/amplicon/primer_trimmer.h"
#include <algorithm>
#include <cctype>

namespace winalign {
namespace amplicon {

/**
 * Implementation of primer trimmer
 */
class PrimerTrimmer::Impl {
public:
    PrimerTrimParams params_;
    std::vector<AmpliconTarget> targets_;
    Stats stats_;

    Impl(const PrimerTrimParams& params)
        : params_(params) {
        reset_stats();
    }

    void reset_stats() {
        stats_ = Stats{0, 0, 0, 0, 0, 0};
    }

    PrimerTrimResult trim(const std::string& sequence, const std::string& quality) {
        PrimerTrimResult result;
        result.trimmed_sequence = sequence;
        result.trimmed_quality = quality;

        stats_.total_reads++;

        if (targets_.empty()) {
            return result;  // No primers to trim
        }

        // Try to find matching primers
        int best_fwd_pos = -1;
        int best_rev_pos = -1;
        uint32_t best_fwd_mismatches = params_.max_mismatches + 1;
        uint32_t best_rev_mismatches = params_.max_mismatches + 1;
        std::string best_amplicon_id;

        for (const auto& target : targets_) {
            // Check forward primer at start of read
            auto [fwd_pos, fwd_mm] = find_primer(
                sequence, target.primer_fwd,
                params_.max_mismatches, params_.min_overlap
            );

            if (fwd_pos == 0 && fwd_mm < best_fwd_mismatches) {
                best_fwd_pos = fwd_pos;
                best_fwd_mismatches = fwd_mm;
                best_amplicon_id = target.amplicon_id;
            }

            // Check reverse primer at end of read
            if (params_.trim_both_ends) {
                std::string rev_primer = target.primer_rev;
                if (params_.check_reverse_complement) {
                    rev_primer = reverse_complement(rev_primer);
                }

                // Look for reverse primer near end of sequence
                int search_start = std::max(0, static_cast<int>(sequence.length()) -
                                           static_cast<int>(rev_primer.length()) - 10);
                std::string end_region = sequence.substr(search_start);

                auto [rev_pos, rev_mm] = find_primer(
                    end_region, rev_primer,
                    params_.max_mismatches, params_.min_overlap
                );

                if (rev_pos >= 0 && rev_mm < best_rev_mismatches) {
                    best_rev_pos = search_start + rev_pos;
                    best_rev_mismatches = rev_mm;
                }
            }
        }

        // Trim sequence
        size_t start = 0;
        size_t end = sequence.length();

        if (best_fwd_pos == 0 && best_fwd_mismatches <= params_.max_mismatches) {
            start = targets_[0].primer_fwd.length();
            result.fwd_found = true;
            result.fwd_trim_len = start;
            stats_.fwd_primer_found++;
        }

        if (best_rev_pos >= 0 && best_rev_mismatches <= params_.max_mismatches) {
            end = best_rev_pos;
            result.rev_found = true;
            result.rev_trim_len = sequence.length() - end;
            stats_.rev_primer_found++;
        }

        if (result.fwd_found && result.rev_found) {
            stats_.both_found++;
        } else if (!result.fwd_found && !result.rev_found) {
            stats_.neither_found++;
        }

        // Perform trimming
        if (start < end) {
            result.trimmed_sequence = sequence.substr(start, end - start);
            if (!quality.empty() && quality.length() == sequence.length()) {
                result.trimmed_quality = quality.substr(start, end - start);
            }
            result.amplicon_id = best_amplicon_id;

            stats_.total_bases_trimmed += (sequence.length() - result.trimmed_sequence.length());
        } else {
            // Invalid trimming - primers overlap, keep original
            result.trimmed_sequence = sequence;
            result.trimmed_quality = quality;
        }

        return result;
    }
};

// Public interface

PrimerTrimmer::PrimerTrimmer(const PrimerTrimParams& params)
    : pimpl_(std::make_unique<Impl>(params)) {}

PrimerTrimmer::~PrimerTrimmer() = default;

void PrimerTrimmer::set_targets(const std::vector<AmpliconTarget>& targets) {
    pimpl_->targets_ = targets;
}

PrimerTrimResult PrimerTrimmer::trim(
    const std::string& sequence,
    const std::string& quality
) {
    return pimpl_->trim(sequence, quality);
}

PrimerTrimResult PrimerTrimmer::trim_cluster(ReadCluster& cluster) {
    auto result = pimpl_->trim(cluster.consensus_sequence, cluster.consensus_quality);

    // Update cluster
    cluster.consensus_sequence = result.trimmed_sequence;
    cluster.consensus_quality = result.trimmed_quality;

    return result;
}

PrimerTrimmer::Stats PrimerTrimmer::get_stats() const {
    return pimpl_->stats_;
}

void PrimerTrimmer::reset_stats() {
    pimpl_->reset_stats();
}

// Utility functions

std::string reverse_complement(const std::string& seq) {
    std::string rc;
    rc.reserve(seq.length());

    for (auto it = seq.rbegin(); it != seq.rend(); ++it) {
        char base = std::toupper(*it);
        switch (base) {
            case 'A': rc += 'T'; break;
            case 'T': rc += 'A'; break;
            case 'C': rc += 'G'; break;
            case 'G': rc += 'C'; break;
            case 'N': rc += 'N'; break;
            default: rc += base;
        }
    }

    return rc;
}

std::pair<int32_t, uint32_t> find_primer(
    const std::string& sequence,
    const std::string& primer,
    uint32_t max_mismatches,
    uint32_t min_overlap
) {
    if (primer.length() < min_overlap || sequence.empty()) {
        return {-1, max_mismatches + 1};
    }

    // Search for primer at beginning of sequence
    const size_t search_len = std::min(sequence.length(), primer.length() + 5);

    int32_t best_pos = -1;
    uint32_t best_mismatches = max_mismatches + 1;

    // Try different start positions (allow small offset)
    for (size_t offset = 0; offset < std::min(size_t(5), search_len); ++offset) {
        uint32_t mismatches = 0;
        size_t matches = 0;

        for (size_t i = 0; i < primer.length() && (offset + i) < sequence.length(); ++i) {
            char s = std::toupper(sequence[offset + i]);
            char p = std::toupper(primer[i]);

            if (s == p || s == 'N' || p == 'N') {
                matches++;
            } else {
                mismatches++;
            }
        }

        if (matches >= min_overlap && mismatches < best_mismatches) {
            best_pos = offset;
            best_mismatches = mismatches;
        }
    }

    return {best_pos, best_mismatches};
}

} // namespace amplicon
} // namespace winalign
