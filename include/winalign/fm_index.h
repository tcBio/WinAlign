#ifndef WINALIGN_FM_INDEX_H
#define WINALIGN_FM_INDEX_H

#include "common.h"
#include <memory>
#include <vector>

namespace winalign {

/**
 * @brief Lightweight host view of the FM-index for device transfer.
 */
struct FMIndexView {
    const uint8_t* bwt = nullptr;
    size_t length = 0;
    const uint64_t* c_table = nullptr;
    const uint64_t* occ_table = nullptr;
    size_t occ_entries = 0;
    const uint64_t* suffix_array = nullptr;
    size_t suffix_length = 0;
    uint32_t occ_interval = 0;
};

/**
 * @brief FM-Index for fast substring search
 *
 * Implements the FM-Index data structure based on the Burrows-Wheeler Transform (BWT).
 * Used for efficient seed matching during alignment.
 */
class FMIndex {
public:
    /**
     * @brief Construct a new FMIndex
     */
    FMIndex();

    /**
     * @brief Destroy the FMIndex
     */
    ~FMIndex();

    // Disable copy
    FMIndex(const FMIndex&) = delete;
    FMIndex& operator=(const FMIndex&) = delete;

    // Enable move
    FMIndex(FMIndex&&) noexcept;
    FMIndex& operator=(FMIndex&&) noexcept;

    /**
     * @brief Build FM-index from reference sequence
     * @param sequence Reference sequence (concatenated)
     * @param length Sequence length
     * @return Result with error code
     */
    Result<bool> build(const char* sequence, size_t length);

    /**
     * @brief Search for exact matches of pattern
     * @param pattern Query pattern
     * @param length Pattern length
     * @param matches Output vector of match positions
     * @return Number of matches found
     */
    size_t search(const char* pattern,
                  size_t length,
                  std::vector<Position>& matches,
                  size_t max_results = 1024) const;

    /**
     * @brief Count occurrences of pattern (faster than search)
     * @param pattern Query pattern
     * @param length Pattern length
     * @return Number of occurrences
     */
    size_t count(const char* pattern, size_t length) const;

    /**
     * @brief Get BWT data for GPU transfer
     * @return Pointer to BWT data
     */
    const uint8_t* get_bwt() const;

    /**
     * @brief Get C table for GPU transfer
     * @return Pointer to C table
     */
    const uint64_t* get_c_table() const;

    /**
     * @brief Get occurrence table for GPU transfer
     * @return Pointer to occurrence table
     */
    const uint64_t* get_occ_table() const;

    /**
     * @brief Get total size of FM-index data
     * @return Size in bytes
     */
    size_t get_data_size() const;

    /**
     * @brief Get suffix array pointer (for mapping matches to coordinates)
     */
    const uint64_t* get_suffix_array() const;

    /**
     * @brief Number of suffix array entries.
     */
    size_t get_suffix_array_length() const;

    /**
     * @brief Get occurrence table interval (checkpoint spacing).
     */
    uint32_t get_occ_interval() const;

    /**
     * @brief Convenience view of all FM-index buffers.
     */
    FMIndexView get_view() const;

    /**
     * @brief Get reference length
     * @return Length of indexed sequence
     */
    size_t get_length() const;

    /**
     * @brief Check if index is built
     * @return true if built, false otherwise
     */
    bool is_built() const;

    /**
     * @brief Save index to file
     * @param filename Output file path
     * @return Result with error code
     */
    Result<bool> save(const std::string& filename);

    /**
     * @brief Load index from file
     * @param filename Input file path
     * @return Result with error code
     */
    Result<bool> load(const std::string& filename);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace winalign

#endif // WINALIGN_FM_INDEX_H
