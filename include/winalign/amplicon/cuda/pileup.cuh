#ifndef WINALIGN_AMPLICON_CUDA_PILEUP_CUH
#define WINALIGN_AMPLICON_CUDA_PILEUP_CUH

#include "winalign/amplicon/amplicon_types.h"
#include <cuda_runtime.h>
#include <cstdint>

namespace winalign {
namespace amplicon {
namespace cuda {

/**
 * GPU Pileup Data Structures
 */

// Pileup at a single genomic position
struct PileupPosition {
    uint64_t position;          // Genomic position
    uint32_t ref_count;         // Reads supporting reference
    uint32_t alt_count;         // Reads supporting alternate
    uint32_t total_depth;       // Total coverage
    char ref_base;              // Reference base
    char alt_base;              // Alternate base (most common non-ref)
    uint32_t other_count;       // Other bases (not ref or primary alt)
    float quality_sum;          // Sum of base qualities
};

// Alignment for GPU processing
struct GPUAlignment {
    uint64_t position;          // Alignment start position
    uint32_t read_id;           // Read identifier
    uint32_t read_count;        // Collapsed read count (weight)
    uint32_t read_length;       // Aligned read length
    uint32_t read_offset;       // Offset in read array
    uint8_t mapping_quality;    // MAPQ
    bool is_reverse;            // Reverse strand flag
};

/**
 * Generate pileup at marker positions
 *
 * @param alignments Device array of GPU alignments
 * @param num_alignments Number of alignments
 * @param reads Device array of read sequences
 * @param qualities Device array of quality scores
 * @param reference Device array of reference sequence
 * @param marker_positions Device array of SNP positions to query
 * @param num_markers Number of marker positions
 * @param pileups Device array for output pileup data
 * @param stream CUDA stream for async execution
 * @return CUDA error code
 */
cudaError_t generate_pileup(
    const GPUAlignment* alignments,
    uint32_t num_alignments,
    const char* reads,
    const char* qualities,
    const char* reference,
    const uint64_t* marker_positions,
    uint32_t num_markers,
    PileupPosition* pileups,
    cudaStream_t stream = 0
);

/**
 * Allocate GPU pileup arrays
 */
cudaError_t allocate_pileup_arrays(
    PileupPosition*& pileups,
    uint32_t num_markers
);

/**
 * Free GPU pileup arrays
 */
cudaError_t free_pileup_arrays(
    PileupPosition* pileups
);

} // namespace cuda
} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_CUDA_PILEUP_CUH
