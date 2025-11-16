#ifndef WINALIGN_FASTQ_WORKER_H
#define WINALIGN_FASTQ_WORKER_H

#include "common.h"
#include "fastq_parser.h"
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

namespace winalign {

/**
 * @brief Multi-threaded FASTQ reader for parallel IO
 *
 * Uses a worker thread pool to read and decompress FASTQ batches in parallel,
 * feeding them to the GPU pipeline via a task queue.
 */
class FastqWorker {
public:
    /**
     * @brief Batch of reads ready for processing
     */
    struct ReadBatch {
        std::vector<ReadPair> pairs;
        std::vector<Read> singles;
        size_t count = 0;
        bool eof = false;
        bool is_paired = false;
    };

    /**
     * @brief Callback function when a batch is ready
     */
    using BatchCallback = std::function<void(ReadBatch&&)>;

    /**
     * @brief Construct a new FASTQ Worker
     * @param read1_path Path to read1 FASTQ
     * @param read2_path Path to read2 FASTQ (empty for single-end)
     * @param batch_size Number of reads per batch
     * @param num_workers Number of worker threads (default: 2)
     * @param prefetch_batches Number of batches to prefetch (default: 3)
     */
    FastqWorker(const std::string& read1_path,
                const std::string& read2_path,
                size_t batch_size,
                int num_workers = 2,
                int prefetch_batches = 3);

    /**
     * @brief Destroy the FASTQ Worker
     */
    ~FastqWorker();

    // Disable copy
    FastqWorker(const FastqWorker&) = delete;
    FastqWorker& operator=(const FastqWorker&) = delete;

    /**
     * @brief Start worker threads
     * @return Result with error code
     */
    Result<bool> start();

    /**
     * @brief Get next batch (blocking)
     * @param batch Output batch
     * @return true if batch was retrieved, false if EOF or stopped
     */
    bool get_next_batch(ReadBatch& batch);

    /**
     * @brief Check if worker has reached EOF
     * @return true if EOF, false otherwise
     */
    bool is_eof() const {
        return eof_.load();
    }

    /**
     * @brief Stop worker threads
     */
    void stop();

private:
    void worker_thread();
    void read_batch_internal(ReadBatch& batch);

    std::string read1_path_;
    std::string read2_path_;
    size_t batch_size_;
    int num_workers_;
    int max_prefetch_;
    bool is_paired_;

    // Thread management
    std::vector<std::thread> workers_;
    std::atomic<bool> running_{false};
    std::atomic<bool> eof_{false};
    std::atomic<bool> stop_requested_{false};

    // Batch queue (thread-safe)
    std::queue<ReadBatch> ready_batches_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable space_cv_;

    // Parser (protected by mutex)
    std::unique_ptr<PairedFastqParser> paired_parser_;
    std::unique_ptr<FastqParser> single_parser_;
    std::mutex parser_mutex_;
};

} // namespace winalign

#endif // WINALIGN_FASTQ_WORKER_H
