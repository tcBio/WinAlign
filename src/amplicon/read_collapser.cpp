#include "winalign/amplicon/read_collapser.h"
#include <unordered_map>
#include <algorithm>
#include <numeric>

// xxHash for fast sequence hashing
// Using simple FNV-1a hash for now (can upgrade to xxHash later)
namespace {

uint64_t fnv1a_hash(const std::string& str) {
    uint64_t hash = 14695981039346656037ULL;
    for (char c : str) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Calculate Hamming distance between two sequences
uint32_t hamming_distance(const std::string& seq1, const std::string& seq2) {
    if (seq1.length() != seq2.length()) {
        return UINT32_MAX; // Different lengths
    }

    uint32_t distance = 0;
    for (size_t i = 0; i < seq1.length(); ++i) {
        if (seq1[i] != seq2[i]) {
            distance++;
        }
    }
    return distance;
}

// Merge quality scores - keep higher quality base at each position
std::string merge_quality_scores(
    const std::string& qual1,
    const std::string& qual2,
    const std::string& seq1,
    const std::string& seq2
) {
    if (qual1.length() != qual2.length()) {
        return qual1; // Fallback
    }

    std::string merged = qual1;
    for (size_t i = 0; i < qual1.length(); ++i) {
        // If sequences differ, keep quality from higher quality base
        if (seq1[i] != seq2[i]) {
            if (qual2[i] > qual1[i]) {
                merged[i] = qual2[i];
            }
        } else {
            // If bases match, take average quality (simple approach)
            merged[i] = std::max(qual1[i], qual2[i]);
        }
    }

    return merged;
}

} // anonymous namespace

namespace winalign {
namespace amplicon {

// PIMPL implementation
class ReadCollapser::Impl {
public:
    Impl(uint32_t max_distance) : max_distance_(max_distance) {}

    std::vector<ReadCluster> collapse(const std::vector<Read>& reads) {
        if (reads.empty()) {
            return {};
        }

        // Stage 1: Hash-based exact matching
        std::unordered_map<uint64_t, ReadCluster> exact_clusters;

        for (const auto& read : reads) {
            uint64_t hash = fnv1a_hash(read.sequence);

            auto it = exact_clusters.find(hash);
            if (it != exact_clusters.end()) {
                // Add to existing cluster
                it->second.read_count++;
                it->second.read_ids.push_back(read.id);

                // Update consensus quality (merge with higher quality)
                it->second.consensus_quality = merge_quality_scores(
                    it->second.consensus_quality,
                    read.quality,
                    it->second.consensus_sequence,
                    read.sequence
                );
            } else {
                // Create new cluster
                ReadCluster cluster;
                cluster.consensus_sequence = read.sequence;
                cluster.consensus_quality = read.quality;
                cluster.read_count = 1;
                cluster.read_ids.push_back(read.id);
                cluster.hash = hash;

                // Calculate average quality
                cluster.avg_quality = calculate_avg_quality(read.quality);

                exact_clusters[hash] = std::move(cluster);
            }
        }

        // Stage 2: Merge near-identical clusters (if max_distance > 0)
        std::vector<ReadCluster> clusters;
        clusters.reserve(exact_clusters.size());

        for (auto& pair : exact_clusters) {
            clusters.push_back(std::move(pair.second));
        }

        if (max_distance_ > 0) {
            clusters = merge_similar_clusters(clusters);
        }

        // Update statistics
        stats_.input_reads = reads.size();
        stats_.output_clusters = clusters.size();
        stats_.compression_ratio = static_cast<float>(reads.size()) /
                                   std::max(1u, static_cast<uint32_t>(clusters.size()));

        // Calculate average cluster size
        uint32_t total_count = 0;
        for (const auto& cluster : clusters) {
            total_count += cluster.read_count;
        }
        stats_.avg_cluster_size = static_cast<float>(total_count) / clusters.size();

        return clusters;
    }

    CollapseStats get_stats() const {
        return stats_;
    }

private:
    float calculate_avg_quality(const std::string& quality) {
        if (quality.empty()) return 0.0f;

        uint64_t sum = 0;
        for (char q : quality) {
            sum += static_cast<uint8_t>(q - 33); // PHRED+33
        }
        return static_cast<float>(sum) / quality.length();
    }

    std::vector<ReadCluster> merge_similar_clusters(std::vector<ReadCluster>& clusters) {
        // Simple greedy merging: iterate through clusters and merge those within distance threshold
        // This is O(n²) but acceptable since we already reduced n by 100-1000x

        std::vector<bool> merged(clusters.size(), false);
        std::vector<ReadCluster> result;

        for (size_t i = 0; i < clusters.size(); ++i) {
            if (merged[i]) continue;

            ReadCluster& base_cluster = clusters[i];

            // Look for similar clusters to merge
            for (size_t j = i + 1; j < clusters.size(); ++j) {
                if (merged[j]) continue;

                // Check if sequences are similar
                uint32_t distance = hamming_distance(
                    base_cluster.consensus_sequence,
                    clusters[j].consensus_sequence
                );

                if (distance <= max_distance_) {
                    // Merge cluster j into base_cluster
                    base_cluster.read_count += clusters[j].read_count;
                    base_cluster.read_ids.insert(
                        base_cluster.read_ids.end(),
                        clusters[j].read_ids.begin(),
                        clusters[j].read_ids.end()
                    );

                    // Update consensus (keep sequence from larger cluster)
                    if (clusters[j].read_count > base_cluster.read_count) {
                        base_cluster.consensus_sequence = clusters[j].consensus_sequence;
                        base_cluster.consensus_quality = clusters[j].consensus_quality;
                    }

                    merged[j] = true;
                }
            }

            result.push_back(std::move(base_cluster));
        }

        return result;
    }

    uint32_t max_distance_;
    CollapseStats stats_;
};

// Public interface implementation
ReadCollapser::ReadCollapser(uint32_t max_distance)
    : pimpl_(std::make_unique<Impl>(max_distance)) {}

ReadCollapser::~ReadCollapser() = default;

std::vector<ReadCluster> ReadCollapser::collapse(const std::vector<Read>& reads) {
    return pimpl_->collapse(reads);
}

ReadCollapser::CollapseStats ReadCollapser::get_stats() const {
    return pimpl_->get_stats();
}

} // namespace amplicon
} // namespace winalign
