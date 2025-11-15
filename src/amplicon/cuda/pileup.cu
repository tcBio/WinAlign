#include "winalign/amplicon/cuda/pileup.cuh"
#include <cuda_runtime.h>
#include <cub/cub.cuh>

namespace winalign {
namespace amplicon {
namespace cuda {

// Device function: Get base at position in alignment
__device__ char get_base_at_position(
    const char* reads,
    const GPUAlignment& aln,
    uint64_t genomic_pos
) {
    // Check if position is within this alignment
    if (genomic_pos < aln.position ||
        genomic_pos >= aln.position + aln.read_length) {
        return 'N';
    }

    // Calculate offset in read
    uint32_t read_pos = genomic_pos - aln.position;
    return reads[aln.read_offset + read_pos];
}

// Device function: Get quality at position
__device__ uint8_t get_quality_at_position(
    const char* qualities,
    const GPUAlignment& aln,
    uint64_t genomic_pos
) {
    if (genomic_pos < aln.position ||
        genomic_pos >= aln.position + aln.read_length) {
        return 0;
    }

    uint32_t read_pos = genomic_pos - aln.position;
    // Convert from PHRED+33 to numeric
    return static_cast<uint8_t>(qualities[aln.read_offset + read_pos] - 33);
}

/**
 * Kernel: Generate pileup at marker positions
 *
 * Strategy: One thread per marker position
 * Each thread iterates through all alignments covering that position
 */
__global__ void generate_pileup_kernel(
    const GPUAlignment* alignments,
    uint32_t num_alignments,
    const char* reads,
    const char* qualities,
    const char* reference,
    const uint64_t* marker_positions,
    uint32_t num_markers,
    PileupPosition* pileups
) {
    uint32_t marker_idx = blockIdx.x * blockDim.x + threadIdx.x;

    if (marker_idx >= num_markers) return;

    uint64_t pos = marker_positions[marker_idx];
    char ref_base = reference[pos];

    // Initialize pileup
    PileupPosition pileup;
    pileup.position = pos;
    pileup.ref_base = ref_base;
    pileup.ref_count = 0;
    pileup.alt_count = 0;
    pileup.other_count = 0;
    pileup.total_depth = 0;
    pileup.quality_sum = 0.0f;
    pileup.alt_base = 'N';

    // Count bases at this position
    uint32_t base_counts[5] = {0, 0, 0, 0, 0}; // A, C, G, T, N
    char base_chars[5] = {'A', 'C', 'G', 'T', 'N'};

    // Iterate through alignments
    for (uint32_t i = 0; i < num_alignments; ++i) {
        const GPUAlignment& aln = alignments[i];

        // Check if alignment covers this position
        if (aln.position <= pos && pos < aln.position + aln.read_length) {
            char base = get_base_at_position(reads, aln, pos);
            uint8_t qual = get_quality_at_position(qualities, aln, pos);

            // Weight by read_count (collapsed read multiplicity)
            uint32_t count = aln.read_count;

            // Count by base type
            switch (base) {
                case 'A': case 'a': base_counts[0] += count; break;
                case 'C': case 'c': base_counts[1] += count; break;
                case 'G': case 'g': base_counts[2] += count; break;
                case 'T': case 't': base_counts[3] += count; break;
                default:  base_counts[4] += count; break;
            }

            pileup.total_depth += count;
            pileup.quality_sum += qual * count;
        }
    }

    // Determine ref and alt alleles
    uint32_t ref_idx = 0;
    switch (ref_base) {
        case 'A': case 'a': ref_idx = 0; break;
        case 'C': case 'c': ref_idx = 1; break;
        case 'G': case 'g': ref_idx = 2; break;
        case 'T': case 't': ref_idx = 3; break;
        default:  ref_idx = 4; break;
    }

    pileup.ref_count = base_counts[ref_idx];

    // Find most common alternate allele
    uint32_t max_alt_count = 0;
    uint32_t alt_idx = 4; // Default to N

    for (uint32_t i = 0; i < 4; ++i) {
        if (i != ref_idx && base_counts[i] > max_alt_count) {
            max_alt_count = base_counts[i];
            alt_idx = i;
        }
    }

    pileup.alt_count = max_alt_count;
    pileup.alt_base = base_chars[alt_idx];

    // Other bases (not ref or primary alt)
    pileup.other_count = pileup.total_depth - pileup.ref_count - pileup.alt_count;

    // Write output
    pileups[marker_idx] = pileup;
}

// Host function: Generate pileup
cudaError_t generate_pileup(
    const GPUAlignment* alignments,
    uint32_t num_alignments,
    const char* reads,
    const char* qualities,
    const char* reference,
    const uint64_t* marker_positions,
    uint32_t num_markers,
    PileupPosition* pileups,
    cudaStream_t stream
) {
    if (num_markers == 0 || num_alignments == 0) {
        return cudaSuccess;
    }

    // Launch configuration: one thread per marker
    dim3 block_size(256);
    dim3 grid_size((num_markers + block_size.x - 1) / block_size.x);

    generate_pileup_kernel<<<grid_size, block_size, 0, stream>>>(
        alignments,
        num_alignments,
        reads,
        qualities,
        reference,
        marker_positions,
        num_markers,
        pileups
    );

    return cudaGetLastError();
}

// Allocate pileup arrays
cudaError_t allocate_pileup_arrays(
    PileupPosition*& pileups,
    uint32_t num_markers
) {
    return cudaMalloc(&pileups, num_markers * sizeof(PileupPosition));
}

// Free pileup arrays
cudaError_t free_pileup_arrays(
    PileupPosition* pileups
) {
    if (pileups) {
        return cudaFree(pileups);
    }
    return cudaSuccess;
}

} // namespace cuda
} // namespace amplicon
} // namespace winalign
