#ifndef WINALIGN_AMPLICON_SIMD_OPS_H
#define WINALIGN_AMPLICON_SIMD_OPS_H

#include <cstdint>
#include <string>

#ifdef __AVX2__
#include <immintrin.h>
#define SIMD_ENABLED 1
#else
#define SIMD_ENABLED 0
#endif

namespace winalign {
namespace amplicon {
namespace simd {

/**
 * SIMD-accelerated operations for amplicon processing
 * Uses AVX2 instructions when available, falls back to scalar
 */

/**
 * Fast hamming distance calculation using SIMD
 * Processes 32 bytes at a time with AVX2
 */
uint32_t hamming_distance_simd(const char* seq1, const char* seq2, size_t length);

/**
 * Fast sequence comparison (exact match check)
 * Returns true if sequences match exactly
 */
bool sequences_equal_simd(const char* seq1, const char* seq2, size_t length);

/**
 * Fast primer search with SIMD
 * Searches for primer in sequence, allows mismatches
 * Returns position or -1 if not found
 */
int32_t find_primer_simd(
    const char* sequence,
    size_t seq_len,
    const char* primer,
    size_t primer_len,
    uint32_t max_mismatches
);

/**
 * Fast base counting with SIMD
 * Counts A, C, G, T, N in sequence
 */
struct BaseCounts {
    uint32_t A, C, G, T, N;
};

BaseCounts count_bases_simd(const char* sequence, size_t length);

/**
 * Fast quality score averaging with SIMD
 */
float average_quality_simd(const char* quality, size_t length);

/**
 * Fast reverse complement with SIMD
 */
void reverse_complement_simd(const char* input, char* output, size_t length);

/**
 * Check if SIMD is available
 */
inline bool is_simd_available() {
#ifdef __AVX2__
    return true;
#else
    return false;
#endif
}

/**
 * Get SIMD instruction set name
 */
const char* get_simd_name();

} // namespace simd
} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_SIMD_OPS_H
