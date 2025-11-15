#include "winalign/amplicon/variant_caller.h"
#include "winalign/amplicon/cuda/pileup.cuh"
#include "winalign/amplicon/cuda/variant_calling.cuh"
#include <cuda_runtime.h>
#include <algorithm>
#include <unordered_map>

namespace winalign {
namespace amplicon {

class VariantCaller::Impl {
public:
    Impl(const AmpliconConfig& config)
        : config_(config)
        , d_pileups_(nullptr)
        , d_variants_(nullptr)
    {}

    ~Impl() {
        cleanup();
    }

    std::vector<AmpliconVariant> call_variants(
        const std::vector<AmpliconAlignment>& alignments,
        const std::string& reference
    ) {
        if (alignments.empty()) {
            return {};
        }

        // Step 1: Convert alignments to GPU format
        auto gpu_alignments = convert_to_gpu_alignments(alignments);

        // Step 2: Collect all SNP positions from configured targets
        std::vector<uint64_t> marker_positions;
        std::unordered_map<uint64_t, std::string> position_to_amplicon;

        for (const auto& target : config_.targets) {
            for (uint64_t pos : target.snp_positions) {
                marker_positions.push_back(pos);
                position_to_amplicon[pos] = target.amplicon_id;
            }
        }

        if (marker_positions.empty()) {
            return {};
        }

        // Step 3: Allocate GPU memory
        cuda::GPUAlignment* d_alignments;
        char* d_reads;
        char* d_qualities;
        char* d_reference;
        uint64_t* d_marker_positions;

        size_t total_read_length = calculate_total_read_length(alignments);

        cudaMalloc(&d_alignments, gpu_alignments.size() * sizeof(cuda::GPUAlignment));
        cudaMalloc(&d_reads, total_read_length);
        cudaMalloc(&d_qualities, total_read_length);
        cudaMalloc(&d_reference, reference.length());
        cudaMalloc(&d_marker_positions, marker_positions.size() * sizeof(uint64_t));

        // Copy data to GPU
        cudaMemcpy(d_alignments, gpu_alignments.data(),
                  gpu_alignments.size() * sizeof(cuda::GPUAlignment),
                  cudaMemcpyHostToDevice);

        cudaMemcpy(d_marker_positions, marker_positions.data(),
                  marker_positions.size() * sizeof(uint64_t),
                  cudaMemcpyHostToDevice);

        cudaMemcpy(d_reference, reference.c_str(),
                  reference.length(),
                  cudaMemcpyHostToDevice);

        // Copy reads and qualities
        copy_reads_to_gpu(alignments, d_reads, d_qualities);

        // Step 4: Allocate pileup arrays
        cuda::allocate_pileup_arrays(d_pileups_, marker_positions.size());

        // Step 5: Generate pileup
        cuda::generate_pileup(
            d_alignments,
            gpu_alignments.size(),
            d_reads,
            d_qualities,
            d_reference,
            d_marker_positions,
            marker_positions.size(),
            d_pileups_,
            0  // Default stream
        );

        // Step 6: Allocate variant arrays
        cuda::allocate_variant_arrays(d_variants_, marker_positions.size());

        // Step 7: Call variants
        cuda::VariantCallParams params;
        params.min_depth = config_.min_depth;
        params.min_allele_frequency = config_.min_allele_frequency;
        params.min_base_quality = config_.quality_threshold;
        params.min_mapping_quality = 20;
        params.het_threshold_low = 0.25f;
        params.het_threshold_high = 0.75f;

        uint32_t* d_num_variants;
        cudaMalloc(&d_num_variants, sizeof(uint32_t));

        cuda::call_variants(
            d_pileups_,
            marker_positions.size(),
            params,
            d_variants_,
            d_num_variants,
            0  // Default stream
        );

        // Step 8: Copy results back to host
        uint32_t num_variants;
        cudaMemcpy(&num_variants, d_num_variants,
                  sizeof(uint32_t),
                  cudaMemcpyDeviceToHost);

        std::vector<cuda::GPUVariant> gpu_variants(marker_positions.size());
        cudaMemcpy(gpu_variants.data(), d_variants_,
                  marker_positions.size() * sizeof(cuda::GPUVariant),
                  cudaMemcpyDeviceToHost);

        // Step 9: Convert to AmpliconVariant format
        std::vector<AmpliconVariant> variants;
        variants.reserve(num_variants);

        for (size_t i = 0; i < gpu_variants.size(); ++i) {
            const auto& gv = gpu_variants[i];

            // Only include variants that passed calling
            if (gv.total_depth >= config_.min_depth &&
                gv.allele_frequency >= config_.min_allele_frequency) {

                AmpliconVariant variant;
                variant.position = gv.position;
                variant.ref_allele = gv.ref_allele;
                variant.alt_allele = gv.alt_allele;
                variant.ref_depth = gv.ref_depth;
                variant.alt_depth = gv.alt_depth;
                variant.total_depth = gv.total_depth;
                variant.allele_frequency = gv.allele_frequency;
                variant.quality = gv.quality;

                // Get chromosome from first alignment (simplified)
                if (!alignments.empty()) {
                    // Find which amplicon this position belongs to
                    auto it = position_to_amplicon.find(gv.position);
                    if (it != position_to_amplicon.end()) {
                        variant.amplicon_id = it->second;
                        variant.is_known_marker = true;

                        // Find chromosome from target
                        for (const auto& target : config_.targets) {
                            if (target.amplicon_id == variant.amplicon_id) {
                                variant.chromosome = target.chromosome;
                                break;
                            }
                        }
                    }
                }

                variants.push_back(variant);
            }
        }

        // Step 10: Update statistics
        for (const auto& variant : variants) {
            auto& stats = amplicon_stats_[variant.amplicon_id];
            stats.amplicon_id = variant.amplicon_id;
            stats.variants_called++;
        }

        // Cleanup GPU memory
        cudaFree(d_alignments);
        cudaFree(d_reads);
        cudaFree(d_qualities);
        cudaFree(d_reference);
        cudaFree(d_marker_positions);
        cudaFree(d_num_variants);

        return variants;
    }

    std::vector<AmpliconStats> get_amplicon_stats() const {
        std::vector<AmpliconStats> stats;
        stats.reserve(amplicon_stats_.size());

        for (const auto& pair : amplicon_stats_) {
            stats.push_back(pair.second);
        }

        return stats;
    }

private:
    std::vector<cuda::GPUAlignment> convert_to_gpu_alignments(
        const std::vector<AmpliconAlignment>& alignments
    ) {
        std::vector<cuda::GPUAlignment> gpu_alignments;
        gpu_alignments.reserve(alignments.size());

        uint32_t read_offset = 0;

        for (const auto& aln : alignments) {
            cuda::GPUAlignment gpu_aln;
            gpu_aln.position = aln.position;
            gpu_aln.read_id = 0;  // Not used for now
            gpu_aln.read_count = aln.read_count;
            gpu_aln.read_length = aln.read_length;
            gpu_aln.read_offset = read_offset;
            gpu_aln.mapping_quality = aln.mapping_quality;
            gpu_aln.is_reverse = false;

            gpu_alignments.push_back(gpu_aln);

            read_offset += aln.read_length;
        }

        return gpu_alignments;
    }

    size_t calculate_total_read_length(
        const std::vector<AmpliconAlignment>& alignments
    ) {
        size_t total = 0;
        for (const auto& aln : alignments) {
            total += aln.read_length;
        }
        return total;
    }

    void copy_reads_to_gpu(
        const std::vector<AmpliconAlignment>& alignments,
        char* d_reads,
        char* d_qualities
    ) {
        // This is simplified - in production, would copy actual read data
        // For now, just allocate space (reads would come from ReadCluster)
        // TODO: Integrate with actual read data from pipeline
    }

    void cleanup() {
        if (d_pileups_) {
            cuda::free_pileup_arrays(d_pileups_);
            d_pileups_ = nullptr;
        }

        if (d_variants_) {
            cuda::free_variant_arrays(d_variants_);
            d_variants_ = nullptr;
        }
    }

    AmpliconConfig config_;
    std::unordered_map<std::string, AmpliconStats> amplicon_stats_;

    // GPU arrays
    cuda::PileupPosition* d_pileups_;
    cuda::GPUVariant* d_variants_;
};

// Public interface
VariantCaller::VariantCaller(const AmpliconConfig& config)
    : pimpl_(std::make_unique<Impl>(config))
{}

VariantCaller::~VariantCaller() = default;

std::vector<AmpliconVariant> VariantCaller::call_variants(
    const std::vector<AmpliconAlignment>& alignments,
    const std::string& reference
) {
    return pimpl_->call_variants(alignments, reference);
}

std::vector<AmpliconStats> VariantCaller::get_amplicon_stats() const {
    return pimpl_->get_amplicon_stats();
}

} // namespace amplicon
} // namespace winalign
