#ifndef WINALIGN_BAM_WRITER_H
#define WINALIGN_BAM_WRITER_H

#include "common.h"
#include <memory>

namespace winalign {

/**
 * @brief BAM file writer using htslib
 *
 * Writes alignment results to BAM format with compression and indexing.
 */
class BamWriter {
public:
    /**
     * @brief Construct a new Bam Writer
     * @param filename Output BAM file path
     * @param reference_sequences Map of reference sequence names to lengths
     */
    BamWriter(const std::string& filename,
              const std::map<std::string, uint64_t>& reference_sequences);

    /**
     * @brief Destroy the Bam Writer
     */
    ~BamWriter();

    // Disable copy
    BamWriter(const BamWriter&) = delete;
    BamWriter& operator=(const BamWriter&) = delete;

    /**
     * @brief Open BAM file for writing
     * @return Result with error code
     */
    Result<bool> open();

    /**
     * @brief Close BAM file
     */
    void close();

    /**
     * @brief Check if file is open
     * @return true if open, false otherwise
     */
    bool is_open() const;

    /**
     * @brief Write single alignment
     * @param alignment Alignment to write
     * @param read Read information
     * @return Result with error code
     */
    Result<bool> write(const Alignment& alignment, const Read& read);

    /**
     * @brief Write batch of alignments
     * @param alignments Vector of alignments
     * @param reads Vector of corresponding reads
     * @return Number of alignments written
     */
    size_t write_batch(const std::vector<Alignment>& alignments,
                      const std::vector<Read>& reads);

    /**
     * @brief Set compression level
     * @param level Compression level (0-9, default 6)
     */
    void set_compression_level(int level);

    /**
     * @brief Set number of threads for compression
     * @param threads Number of threads
     */
    void set_threads(int threads);

    /**
     * @brief Enable/disable index generation
     * @param enable true to enable index generation
     */
    void enable_index(bool enable);

    /**
     * @brief Get total alignments written
     * @return Number of alignments written
     */
    uint64_t total_written() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace winalign

#endif // WINALIGN_BAM_WRITER_H
