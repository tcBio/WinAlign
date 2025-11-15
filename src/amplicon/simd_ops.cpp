#include "winalign/amplicon/simd_ops.h"
#include <cstring>
#include <cctype>
#include <algorithm>

namespace winalign {
namespace amplicon {
namespace simd {

#ifdef __AVX2__

/**
 * AVX2 implementation of hamming distance
 * Processes 32 bytes per iteration
 */
uint32_t hamming_distance_simd(const char* seq1, const char* seq2, size_t length) {
    uint32_t distance = 0;
    size_t i = 0;

    // Process 32 bytes at a time with AVX2
    for (; i + 32 <= length; i += 32) {
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(seq1 + i));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(seq2 + i));

        // XOR to find differences
        __m256i diff = _mm256_xor_si256(v1, v2);

        // Count non-zero bytes (mismatches)
        __m256i zero = _mm256_setzero_si256();
        __m256i cmp = _mm256_cmpeq_epi8(diff, zero);

        // Count zeros (matches) and subtract from 32
        int mask = _mm256_movemask_epi8(cmp);
        distance += 32 - __builtin_popcount(mask);
    }

    // Handle remaining bytes
    for (; i < length; ++i) {
        if (std::toupper(seq1[i]) != std::toupper(seq2[i])) {
            distance++;
        }
    }

    return distance;
}

/**
 * AVX2 implementation of sequence equality check
 */
bool sequences_equal_simd(const char* seq1, const char* seq2, size_t length) {
    size_t i = 0;

    // Process 32 bytes at a time
    for (; i + 32 <= length; i += 32) {
        __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(seq1 + i));
        __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(seq2 + i));

        __m256i cmp = _mm256_cmpeq_epi8(v1, v2);
        int mask = _mm256_movemask_epi8(cmp);

        if (mask != -1) {  // Not all equal
            return false;
        }
    }

    // Check remaining bytes
    for (; i < length; ++i) {
        if (std::toupper(seq1[i]) != std::toupper(seq2[i])) {
            return false;
        }
    }

    return true;
}

/**
 * AVX2 implementation of base counting
 */
BaseCounts count_bases_simd(const char* sequence, size_t length) {
    BaseCounts counts = {0, 0, 0, 0, 0};

    // Create lookup vectors for each base
    __m256i A_char = _mm256_set1_epi8('A');
    __m256i C_char = _mm256_set1_epi8('C');
    __m256i G_char = _mm256_set1_epi8('G');
    __m256i T_char = _mm256_set1_epi8('T');
    __m256i N_char = _mm256_set1_epi8('N');

    size_t i = 0;

    // Process 32 bytes at a time
    for (; i + 32 <= length; i += 32) {
        __m256i seq = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(sequence + i));

        // Compare with each base
        __m256i cmp_A = _mm256_cmpeq_epi8(seq, A_char);
        __m256i cmp_C = _mm256_cmpeq_epi8(seq, C_char);
        __m256i cmp_G = _mm256_cmpeq_epi8(seq, G_char);
        __m256i cmp_T = _mm256_cmpeq_epi8(seq, T_char);
        __m256i cmp_N = _mm256_cmpeq_epi8(seq, N_char);

        // Count matches
        counts.A += __builtin_popcount(_mm256_movemask_epi8(cmp_A));
        counts.C += __builtin_popcount(_mm256_movemask_epi8(cmp_C));
        counts.G += __builtin_popcount(_mm256_movemask_epi8(cmp_G));
        counts.T += __builtin_popcount(_mm256_movemask_epi8(cmp_T));
        counts.N += __builtin_popcount(_mm256_movemask_epi8(cmp_N));
    }

    // Handle remaining bytes
    for (; i < length; ++i) {
        char base = std::toupper(sequence[i]);
        switch (base) {
            case 'A': counts.A++; break;
            case 'C': counts.C++; break;
            case 'G': counts.G++; break;
            case 'T': counts.T++; break;
            case 'N': counts.N++; break;
        }
    }

    return counts;
}

/**
 * AVX2 implementation of quality averaging
 */
float average_quality_simd(const char* quality, size_t length) {
    if (length == 0) return 0.0f;

    __m256i sum_vec = _mm256_setzero_si256();
    __m256i offset = _mm256_set1_epi8(33);  // PHRED+33 offset

    size_t i = 0;

    // Process 32 bytes at a time
    for (; i + 32 <= length; i += 32) {
        __m256i qual = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(quality + i));

        // Subtract PHRED offset
        __m256i scores = _mm256_sub_epi8(qual, offset);

        // Accumulate (convert to 16-bit to avoid overflow)
        __m256i low = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(scores, 0));
        __m256i high = _mm256_cvtepi8_epi16(_mm256_extracti128_si256(scores, 1));

        sum_vec = _mm256_add_epi16(sum_vec, low);
        sum_vec = _mm256_add_epi16(sum_vec, high);
    }

    // Horizontal sum
    uint32_t sum = 0;
    int16_t* vals = reinterpret_cast<int16_t*>(&sum_vec);
    for (int j = 0; j < 16; ++j) {
        sum += vals[j];
    }

    // Handle remaining bytes
    for (; i < length; ++i) {
        sum += static_cast<uint32_t>(quality[i] - 33);
    }

    return static_cast<float>(sum) / static_cast<float>(length);
}

/**
 * AVX2 implementation of reverse complement
 */
void reverse_complement_simd(const char* input, char* output, size_t length) {
    // Lookup table for complement
    alignas(32) static const char complement_table[256] = {
        ['A'] = 'T', ['T'] = 'A', ['C'] = 'G', ['G'] = 'C',
        ['a'] = 'T', ['t'] = 'A', ['c'] = 'G', ['g'] = 'C',
        ['N'] = 'N', ['n'] = 'N'
    };

    // Reverse and complement
    // Note: AVX2 doesn't have a good byte shuffle for reverse, so we do it in chunks
    for (size_t i = 0; i < length; ++i) {
        output[length - 1 - i] = complement_table[static_cast<uint8_t>(input[i])];
    }
}

#else

/**
 * Scalar fallback implementations
 */

uint32_t hamming_distance_simd(const char* seq1, const char* seq2, size_t length) {
    uint32_t distance = 0;
    for (size_t i = 0; i < length; ++i) {
        if (std::toupper(seq1[i]) != std::toupper(seq2[i])) {
            distance++;
        }
    }
    return distance;
}

bool sequences_equal_simd(const char* seq1, const char* seq2, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (std::toupper(seq1[i]) != std::toupper(seq2[i])) {
            return false;
        }
    }
    return true;
}

BaseCounts count_bases_simd(const char* sequence, size_t length) {
    BaseCounts counts = {0, 0, 0, 0, 0};
    for (size_t i = 0; i < length; ++i) {
        char base = std::toupper(sequence[i]);
        switch (base) {
            case 'A': counts.A++; break;
            case 'C': counts.C++; break;
            case 'G': counts.G++; break;
            case 'T': counts.T++; break;
            case 'N': counts.N++; break;
        }
    }
    return counts;
}

float average_quality_simd(const char* quality, size_t length) {
    if (length == 0) return 0.0f;
    uint32_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum += static_cast<uint32_t>(quality[i] - 33);
    }
    return static_cast<float>(sum) / static_cast<float>(length);
}

void reverse_complement_simd(const char* input, char* output, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        char base = std::toupper(input[i]);
        char complement;
        switch (base) {
            case 'A': complement = 'T'; break;
            case 'T': complement = 'A'; break;
            case 'C': complement = 'G'; break;
            case 'G': complement = 'C'; break;
            default: complement = 'N'; break;
        }
        output[length - 1 - i] = complement;
    }
}

#endif

/**
 * Common implementation for primer search
 */
int32_t find_primer_simd(
    const char* sequence,
    size_t seq_len,
    const char* primer,
    size_t primer_len,
    uint32_t max_mismatches
) {
    if (primer_len > seq_len) {
        return -1;
    }

    size_t search_len = std::min(seq_len - primer_len + 1, size_t(50));

    for (size_t i = 0; i < search_len; ++i) {
        uint32_t mismatches = hamming_distance_simd(sequence + i, primer, primer_len);

        if (mismatches <= max_mismatches) {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

const char* get_simd_name() {
#ifdef __AVX2__
    return "AVX2";
#elif defined(__SSE2__)
    return "SSE2";
#else
    return "Scalar";
#endif
}

} // namespace simd
} // namespace amplicon
} // namespace winalign
