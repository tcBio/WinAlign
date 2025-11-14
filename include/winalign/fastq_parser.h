#ifndef WINALIGN_FASTQ_PARSER_H
#define WINALIGN_FASTQ_PARSER_H

#include "common.h"
#include <fstream>
#include <memory>

namespace winalign {

/**
 * @brief FASTQ/FASTQ.gz parser with streaming support
 *
 * Parses FASTQ files with optional gzip compression.
 * Uses stream-based processing to minimize memory usage.
 */
class FastqParser {
public:
    /**
     * @brief Construct a new Fastq Parser
     * @param filename Path to FASTQ or FASTQ.gz file
     * @param is_gzipped Whether the file is gzip compressed (auto-detected if not specified)
     */
    explicit FastqParser(const std::string& filename, bool is_gzipped = false);

    /**
     * @brief Destroy the Fastq Parser
     */
    ~FastqParser();

    // Disable copy
    FastqParser(const FastqParser&) = delete;
    FastqParser& operator=(const FastqParser&) = delete;

    // Enable move
    FastqParser(FastqParser&&) noexcept;
    FastqParser& operator=(FastqParser&&) noexcept;

    /**
     * @brief Open the FASTQ file
     * @return Result with error code
     */
    Result<bool> open();

    /**
     * @brief Close the FASTQ file
     */
    void close();

    /**
     * @brief Check if file is open
     * @return true if open, false otherwise
     */
    bool is_open() const;

    /**
     * @brief Read next read from file
     * @param read Output read
     * @return true if read was successful, false if EOF or error
     */
    bool next(Read& read);

    /**
     * @brief Read next batch of reads
     * @param reads Output vector of reads
     * @param batch_size Maximum number of reads to read
     * @return Number of reads read
     */
    size_t next_batch(std::vector<Read>& reads, size_t batch_size = DEFAULT_BATCH_SIZE);

    /**
     * @brief Get total reads parsed
     * @return Number of reads parsed so far
     */
    uint64_t total_reads() const;

    /**
     * @brief Check if end of file reached
     * @return true if EOF, false otherwise
     */
    bool eof() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * @brief Paired-end FASTQ parser
 *
 * Parses two FASTQ files simultaneously for paired-end reads.
 */
class PairedFastqParser {
public:
    /**
     * @brief Construct a new Paired Fastq Parser
     * @param filename1 Path to first FASTQ file (R1)
     * @param filename2 Path to second FASTQ file (R2)
     */
    PairedFastqParser(const std::string& filename1, const std::string& filename2);

    /**
     * @brief Destroy the Paired Fastq Parser
     */
    ~PairedFastqParser();

    // Disable copy
    PairedFastqParser(const PairedFastqParser&) = delete;
    PairedFastqParser& operator=(const PairedFastqParser&) = delete;

    /**
     * @brief Open both FASTQ files
     * @return Result with error code
     */
    Result<bool> open();

    /**
     * @brief Close both FASTQ files
     */
    void close();

    /**
     * @brief Read next read pair
     * @param pair Output read pair
     * @return true if successful, false if EOF or error
     */
    bool next(ReadPair& pair);

    /**
     * @brief Read next batch of read pairs
     * @param pairs Output vector of read pairs
     * @param batch_size Maximum number of pairs to read
     * @return Number of pairs read
     */
    size_t next_batch(std::vector<ReadPair>& pairs, size_t batch_size = DEFAULT_BATCH_SIZE);

    /**
     * @brief Get total read pairs parsed
     * @return Number of pairs parsed so far
     */
    uint64_t total_pairs() const;

    /**
     * @brief Check if end of file reached
     * @return true if EOF on both files, false otherwise
     */
    bool eof() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace winalign

#endif // WINALIGN_FASTQ_PARSER_H
