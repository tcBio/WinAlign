#ifndef WINALIGN_CUDA_SEEDING_CUH
#define WINALIGN_CUDA_SEEDING_CUH

#include <cuda_runtime.h>
#include <cstdint>

namespace winalign {
namespace cuda {

/**
 * @brief GPU seed information
 */
struct Seed {
    uint64_t position;      // Position in reference
    uint32_t read_id;       // Read ID this seed belongs to
    uint32_t read_offset;   // Offset in read
    uint16_t length;        // Seed length
    uint16_t mismatches;    // Number of mismatches
};

/**
 * @brief GPU read batch for seeding
 */
struct ReadBatch {
    char* sequences;        // Device pointer to read sequences
    uint32_t* lengths;      // Device pointer to read lengths
    uint32_t* offsets;      // Device pointer to sequence offsets
    uint32_t num_reads;     // Number of reads in batch

    ReadBatch() : sequences(nullptr), lengths(nullptr),
                  offsets(nullptr), num_reads(0) {}
};

/**
 * @brief GPU FM-index structure
 */
struct FMIndex {
    uint8_t* bwt;           // Burrows-Wheeler Transform
    uint64_t* c_table;      // C table (cumulative character counts)
    uint64_t* occ_table;    // Occurrence table
    uint64_t length;        // Total length
    uint64_t* suffix_array; // Full suffix array
    size_t occ_entries;     // Number of occurrence checkpoints * 5
    size_t suffix_length;   // Entries in suffix array
    uint32_t occ_interval;  // Checkpoint interval

    FMIndex() : bwt(nullptr), c_table(nullptr),
                occ_table(nullptr), length(0),
                suffix_array(nullptr), occ_entries(0),
                suffix_length(0), occ_interval(0) {}
};

struct HostFMIndexView {
    const uint8_t* bwt = nullptr;
    uint64_t length = 0;
    const uint64_t* c_table = nullptr;
    const uint64_t* occ_table = nullptr;
    size_t occ_entries = 0;
    const uint64_t* suffix_array = nullptr;
    size_t suffix_length = 0;
    uint32_t occ_interval = 0;
};

/**
 * @brief Extract k-mers and find seeds
 *
 * @param reads Input read batch on device
 * @param fm_index FM-index on device
 * @param seeds Output seeds on device
 * @param max_seeds Maximum seeds per read
 * @param kmer_size K-mer size
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t extract_seeds(
    const ReadBatch& reads,
    const FMIndex& fm_index,
    Seed* seeds,
    uint32_t max_seeds,
    uint32_t kmer_size,
    cudaStream_t stream = 0
);

/**
 * @brief Filter and rank seeds by quality
 *
 * @param seeds Input/output seeds on device
 * @param num_seeds Number of seeds
 * @param max_seeds_per_read Maximum seeds to keep per read
 * @param stream CUDA stream for async execution
 * @return cudaError_t CUDA error code
 */
cudaError_t filter_seeds(
    Seed* seeds,
    uint32_t num_seeds,
    uint32_t max_seeds_per_read,
    cudaStream_t stream = 0
);

/**
 * @brief Allocate read batch on device
 *
 * @param batch Output read batch
 * @param max_reads Maximum number of reads
 * @param max_read_length Maximum read length
 * @return cudaError_t CUDA error code
 */
cudaError_t allocate_read_batch(
    ReadBatch& batch,
    uint32_t max_reads,
    uint32_t max_read_length
);

/**
 * @brief Free read batch on device
 *
 * @param batch Read batch to free
 * @return cudaError_t CUDA error code
 */
cudaError_t free_read_batch(ReadBatch& batch);

/**
 * @brief Allocate FM-index on device
 *
 * @param fm_index Output FM-index
 * @param length Reference length
 * @return cudaError_t CUDA error code
 */
cudaError_t allocate_fm_index(
    FMIndex& fm_index,
    uint64_t length,
    size_t occ_entries,
    size_t suffix_length
);

/**
 * @brief Free FM-index on device
 *
 * @param fm_index FM-index to free
 * @return cudaError_t CUDA error code
 */
cudaError_t free_fm_index(FMIndex& fm_index);

/**
 * @brief Copy FM-index to device
 *
 * @param dst Device FM-index
 * @param src Host FM-index data
 * @param stream CUDA stream for async copy
 * @return cudaError_t CUDA error code
 */
cudaError_t copy_fm_index_to_device(
    FMIndex& dst,
    const HostFMIndexView& src,
    cudaStream_t stream = 0
);

} // namespace cuda
} // namespace winalign

#endif // WINALIGN_CUDA_SEEDING_CUH
