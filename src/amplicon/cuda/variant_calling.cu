#include "winalign/amplicon/cuda/variant_calling.cuh"
#include <cuda_runtime.h>
#include <cub/cub.cuh>
#include <cmath>

namespace winalign {
namespace amplicon {
namespace cuda {

/**
 * Device function: Calculate binomial probability
 * P(k successes | n trials, probability p)
 */
__device__ float binomial_probability(uint32_t k, uint32_t n, float p) {
    if (n == 0) return 0.0f;
    if (k > n) return 0.0f;

    // Use log probabilities to avoid overflow
    // log P(k|n,p) = log(n choose k) + k*log(p) + (n-k)*log(1-p)

    // Approximate log(n choose k) using Stirling's approximation
    float log_choose = 0.0f;
    if (k > 0 && k < n) {
        log_choose = lgammaf(n + 1.0f) - lgammaf(k + 1.0f) - lgammaf(n - k + 1.0f);
    }

    float log_p = k * logf(fmaxf(p, 1e-10f));
    float log_1_minus_p = (n - k) * logf(fmaxf(1.0f - p, 1e-10f));

    float log_prob = log_choose + log_p + log_1_minus_p;
    return expf(log_prob);
}

/**
 * Device function: Calculate PHRED-scaled quality (improved Bayesian model)
 */
__device__ uint8_t calculate_phred_quality(float allele_frequency, uint32_t depth) {
    // Improved quality model using Bayesian statistics
    // Q = -10 * log10(P(error))

    if (depth == 0) return 0;

    // Sequencing error rate (assume 0.01 = Q20)
    const float error_rate = 0.01f;

    // Calculate likelihoods for different genotypes
    // P(data | hom ref)
    float p_data_hom_ref = binomial_probability(
        static_cast<uint32_t>(allele_frequency * depth), depth, error_rate
    );

    // P(data | het)
    float p_data_het = binomial_probability(
        static_cast<uint32_t>(allele_frequency * depth), depth, 0.5f
    );

    // P(data | hom alt)
    float p_data_hom_alt = binomial_probability(
        static_cast<uint32_t>(allele_frequency * depth), depth, 1.0f - error_rate
    );

    // Determine most likely genotype
    float max_likelihood = fmaxf(p_data_hom_ref, fmaxf(p_data_het, p_data_hom_alt));

    // Alternative likelihood (second best)
    float alt_likelihood;
    if (max_likelihood == p_data_hom_ref) {
        alt_likelihood = fmaxf(p_data_het, p_data_hom_alt);
    } else if (max_likelihood == p_data_het) {
        alt_likelihood = fmaxf(p_data_hom_ref, p_data_hom_alt);
    } else {
        alt_likelihood = fmaxf(p_data_hom_ref, p_data_het);
    }

    // Quality is based on likelihood ratio
    float quality;
    if (alt_likelihood > 1e-10f) {
        float likelihood_ratio = max_likelihood / alt_likelihood;
        quality = -10.0f * log10f(1.0f / fmaxf(likelihood_ratio, 1.0f));
    } else {
        // Very confident - use depth-based quality
        quality = 10.0f * log10f(static_cast<float>(depth));
    }

    // Cap at 60 (standard max PHRED quality)
    quality = fminf(quality, 60.0f);
    quality = fmaxf(quality, 0.0f);

    return static_cast<uint8_t>(quality);
}

/**
 * Device function: Determine genotype from allele frequency
 */
__device__ uint8_t determine_genotype(
    float allele_frequency,
    float het_low,
    float het_high
) {
    if (allele_frequency < het_low) {
        return 0;  // 0/0 (homozygous reference)
    } else if (allele_frequency > het_high) {
        return 2;  // 1/1 (homozygous alternate)
    } else {
        return 1;  // 0/1 (heterozygous)
    }
}

/**
 * Kernel: Call variants from pileup data
 */
__global__ void call_variants_kernel(
    const PileupPosition* pileups,
    uint32_t num_pileups,
    VariantCallParams params,
    GPUVariant* variants,
    uint32_t* variant_flags  // 1 if variant called, 0 otherwise
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= num_pileups) return;

    const PileupPosition& pileup = pileups[idx];

    // Initialize as no variant
    variant_flags[idx] = 0;

    // Check minimum depth
    if (pileup.total_depth < params.min_depth) {
        return;
    }

    // Calculate allele frequency
    float allele_frequency = 0.0f;
    if (pileup.total_depth > 0) {
        allele_frequency = static_cast<float>(pileup.alt_count) /
                          static_cast<float>(pileup.total_depth);
    }

    // Check minimum allele frequency
    if (allele_frequency < params.min_allele_frequency) {
        return;
    }

    // Check that we have a valid alternate allele
    if (pileup.alt_base == 'N' || pileup.alt_base == pileup.ref_base) {
        return;
    }

    // Call variant
    GPUVariant variant;
    variant.position = pileup.position;
    variant.ref_allele = pileup.ref_base;
    variant.alt_allele = pileup.alt_base;
    variant.ref_depth = pileup.ref_count;
    variant.alt_depth = pileup.alt_count;
    variant.total_depth = pileup.total_depth;
    variant.allele_frequency = allele_frequency;

    // Determine genotype
    variant.genotype = determine_genotype(
        allele_frequency,
        params.het_threshold_low,
        params.het_threshold_high
    );

    // Calculate quality
    variant.quality = calculate_phred_quality(allele_frequency, pileup.total_depth);

    // Mark as passing
    variant.is_pass = true;

    // Write variant
    variants[idx] = variant;
    variant_flags[idx] = 1;
}

/**
 * Kernel: Calculate variant quality scores
 */
__global__ void calculate_quality_kernel(
    GPUVariant* variants,
    uint32_t num_variants
) {
    uint32_t idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (idx >= num_variants) return;

    GPUVariant& variant = variants[idx];

    // Recalculate quality based on depth and AF
    variant.quality = calculate_phred_quality(
        variant.allele_frequency,
        variant.total_depth
    );
}

// Host function: Call variants
cudaError_t call_variants(
    const PileupPosition* pileups,
    uint32_t num_pileups,
    const VariantCallParams& params,
    GPUVariant* variants,
    uint32_t* num_variants_out,
    cudaStream_t stream
) {
    if (num_pileups == 0) {
        cudaMemset(num_variants_out, 0, sizeof(uint32_t));
        return cudaSuccess;
    }

    // Allocate flags array to mark which positions have variants
    uint32_t* d_variant_flags;
    cudaError_t err = cudaMalloc(&d_variant_flags, num_pileups * sizeof(uint32_t));
    if (err != cudaSuccess) return err;

    cudaMemset(d_variant_flags, 0, num_pileups * sizeof(uint32_t));

    // Launch variant calling kernel
    dim3 block_size(256);
    dim3 grid_size((num_pileups + block_size.x - 1) / block_size.x);

    call_variants_kernel<<<grid_size, block_size, 0, stream>>>(
        pileups,
        num_pileups,
        params,
        variants,
        d_variant_flags
    );

    err = cudaGetLastError();
    if (err != cudaSuccess) {
        cudaFree(d_variant_flags);
        return err;
    }

    // Count total variants using CUB reduction
    void* d_temp_storage = nullptr;
    size_t temp_storage_bytes = 0;

    // Get required temp storage size
    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_variant_flags,
        num_variants_out,
        num_pileups,
        stream
    );

    // Allocate temp storage
    cudaMalloc(&d_temp_storage, temp_storage_bytes);

    // Run reduction
    cub::DeviceReduce::Sum(
        d_temp_storage,
        temp_storage_bytes,
        d_variant_flags,
        num_variants_out,
        num_pileups,
        stream
    );

    // Cleanup
    cudaFree(d_variant_flags);
    cudaFree(d_temp_storage);

    return cudaSuccess;
}

// Calculate variant quality
cudaError_t calculate_variant_quality(
    GPUVariant* variants,
    uint32_t num_variants,
    cudaStream_t stream
) {
    if (num_variants == 0) return cudaSuccess;

    dim3 block_size(256);
    dim3 grid_size((num_variants + block_size.x - 1) / block_size.x);

    calculate_quality_kernel<<<grid_size, block_size, 0, stream>>>(
        variants,
        num_variants
    );

    return cudaGetLastError();
}

// Allocate variant arrays
cudaError_t allocate_variant_arrays(
    GPUVariant*& variants,
    uint32_t max_variants
) {
    return cudaMalloc(&variants, max_variants * sizeof(GPUVariant));
}

// Free variant arrays
cudaError_t free_variant_arrays(
    GPUVariant* variants
) {
    if (variants) {
        return cudaFree(variants);
    }
    return cudaSuccess;
}

} // namespace cuda
} // namespace amplicon
} // namespace winalign
