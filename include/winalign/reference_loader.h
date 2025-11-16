#ifndef WINALIGN_REFERENCE_LOADER_H
#define WINALIGN_REFERENCE_LOADER_H

#include "common.h"
#include "fm_index.h"
#include <map>
#include <memory>
#include <vector>

namespace winalign {

/**
 * @brief Reference sequence information
 */
struct ReferenceSequence {
    std::string name;
    std::string sequence;
    uint64_t length;

    ReferenceSequence() : length(0) {}
};

struct ChromosomeOffset {
    std::string name;
    uint64_t start = 0;   // Global offset in concatenated reference
    uint64_t length = 0;
};

/**
 * @brief Reference genome loader
 *
 * Loads reference genome from FASTA file and builds indices.
 */
class ReferenceLoader {
public:
    /**
     * @brief Construct a new Reference Loader
     * @param fasta_path Path to reference FASTA file
     */
    explicit ReferenceLoader(const std::string& fasta_path);

    /**
     * @brief Destroy the Reference Loader
     */
    ~ReferenceLoader();

    // Disable copy
    ReferenceLoader(const ReferenceLoader&) = delete;
    ReferenceLoader& operator=(const ReferenceLoader&) = delete;

    /**
     * @brief Load reference genome from FASTA
     * @return Result with error code
     */
    Result<bool> load();

    /**
     * @brief Build FM-index for the reference
     * @return Result with error code
     */
    Result<bool> build_index();

    /**
     * @brief Save index to file
     * @param index_path Path to save index
     * @return Result with error code
     */
    Result<bool> save_index(const std::string& index_path);

    /**
     * @brief Load index from file
     * @param index_path Path to load index from
     * @return Result with error code
     */
    Result<bool> load_index(const std::string& index_path);

    /**
     * @brief Get reference sequence by name
     * @param name Sequence name
     * @return Pointer to sequence, or nullptr if not found
     */
    const ReferenceSequence* get_sequence(const std::string& name) const;

    /**
     * @brief Get all reference sequences
     * @return Map of sequence name to ReferenceSequence
     */
    const std::map<std::string, ReferenceSequence>& get_sequences() const;

    /**
     * @brief Get total reference length
     * @return Total length of all sequences
     */
    uint64_t total_length() const;

    /**
     * @brief Get number of sequences
     * @return Number of sequences
     */
    size_t num_sequences() const;

    /**
     * @brief Check if index is built
     * @return true if index is built, false otherwise
     */
    bool is_indexed() const;

    /**
     * @brief Get raw FM-index data for GPU transfer
     * @return Pointer to FM-index data
     */
    const void* get_fm_index_data() const;

    /**
     * @brief Get FM-index data size
     * @return Size in bytes
     */
    size_t get_fm_index_size() const;

    /**
     * @brief Get FM-index object for advanced queries.
     */
    const FMIndex* get_fm_index() const;

    /**
     * @brief Get flattened reference sequence.
     */
    const std::string& concatenated_sequence() const;

    /**
     * @brief Map global position to contig name and local offset.
     */
    bool map_global_position(uint64_t global_pos,
                             std::string& contig,
                             uint64_t& contig_offset) const;

    /**
     * @brief Get chromosome offsets for reporting.
     */
    const std::vector<ChromosomeOffset>& chromosome_offsets() const;

    /**
     * @brief Extract subsequence from global coordinates.
     */
    std::string get_subsequence(uint64_t global_start, uint32_t length) const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace winalign

#endif // WINALIGN_REFERENCE_LOADER_H
