#ifndef WINALIGN_AMPLICON_CUDA_VARIANT_CALLING_CUH
#define WINALIGN_AMPLICON_CUDA_VARIANT_CALLING_CUH

#include "pileup.cuh"
#include "winalign/amplicon/amplicon_types.h"
#include <cuda_runtime.h>

namespace winalign {
namespace amplicon {
namespace cuda {

/**
 * GPU Variant Data Structure
 */
struct GPUVariant {
    uint64_t position;          // Genomic position
    char ref_allele;            // Reference allele
    char alt_allele;            // Alternate allele
    uint32_t ref_depth;         // Reference allele depth
    uint32_t alt_depth;         // Alternate allele depth
    uint32_t total_depth;       // Total depth
    float allele_frequency;     // AF = alt_depth / total_depth
    uint8_t quality;            // Variant quality (PHRED-scaled)
    uint8_t genotype;           // 0=0/0, 1=0/1, 2=1/1
    bool is_pass;               // Passed filters
};

/**
 * Variant calling parameters
 */
struct VariantCallParams {
    uint32_t min_depth;              // Minimum total depth (default: 100)
    float min_allele_frequency;      // Minimum AF for variant (default: 0.01)
    uint8_t min_base_quality;        // Minimum base quality (default: 20)
    uint8_t min_mapping_quality;     // Minimum MAPQ (default: 20)
    float het_threshold_low;         // Lower bound for het (default: 0.25)
    float het_threshold_high;        // Upper bound for het (default: 0.75)
};

/**
 * Call variants from pileup data
 *
 * @param pileups Device array of pileup positions
 * @param num_pileups Number of pileup positions
 * @param params Calling parameters
 * @param variants Device array for output variants
 * @param num_variants_out Device pointer for variant count
 * @param stream CUDA stream
 * @return CUDA error code
 */
cudaError_t call_variants(
    const PileupPosition* pileups,
    uint32_t num_pileups,
    const VariantCallParams& params,
    GPUVariant* variants,
    uint32_t* num_variants_out,
    cudaStream_t stream = 0
);

/**
 * Calculate variant quality scores (PHRED-scaled)
 *
 * @param variants Device array of variants
 * @param num_variants Number of variants
 * @param stream CUDA stream
 * @return CUDA error code
 */
cudaError_t calculate_variant_quality(
    GPUVariant* variants,
    uint32_t num_variants,
    cudaStream_t stream = 0
);

/**
 * Allocate GPU variant arrays
 */
cudaError_t allocate_variant_arrays(
    GPUVariant*& variants,
    uint32_t max_variants
);

/**
 * Free GPU variant arrays
 */
cudaError_t free_variant_arrays(
    GPUVariant* variants
);

} // namespace cuda
} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_CUDA_VARIANT_CALLING_CUH
