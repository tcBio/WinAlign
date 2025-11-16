#include "winalign/fastq_worker.h"
#include "winalign/logger.h"
#include <chrono>

namespace winalign {

FastqWorker::FastqWorker(const std::string& read1_path,
                         const std::string& read2_path,
                         size_t batch_size,
                         int num_workers,
                         int prefetch_batches)
    : read1_path_(read1_path)
    , read2_path_(read2_path)
    , batch_size_(batch_size)
    , num_workers_(num_workers)
    , max_prefetch_(prefetch_batches)
    , is_paired_(!read2_path.empty())
{
}

FastqWorker::~FastqWorker() {
    stop();
}

Result<bool> FastqWorker::start() {
    if (running_) {
        return Result<bool>(ErrorCode::RUNTIME_ERROR, "Worker already running");
    }

    // Open parsers
    if (is_paired_) {
        paired_parser_ = std::make_unique<PairedFastqParser>(read1_path_, read2_path_);
        auto result = paired_parser_->open();
        if (!result.is_ok()) {
            return result;
        }
    } else {
        single_parser_ = std::make_unique<FastqParser>(read1_path_);
        auto result = single_parser_->open();
        if (!result.is_ok()) {
            return result;
        }
    }

    running_ = true;
    eof_ = false;
    stop_requested_ = false;

    // Start worker threads
    Logger::instance().info("Starting " + std::to_string(num_workers_) +
                           " FASTQ worker threads");
    for (int i = 0; i < num_workers_; ++i) {
        workers_.emplace_back(&FastqWorker::worker_thread, this);
    }

    Logger::instance().info("FASTQ worker threads started");
    return Result<bool>(true);
}

void FastqWorker::stop() {
    if (!running_) {
        return;
    }

    Logger::instance().info("Stopping FASTQ worker threads");
    stop_requested_ = true;
    running_ = false;

    // Wake up all waiting threads
    queue_cv_.notify_all();
    space_cv_.notify_all();

    // Join all worker threads
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    // Close parsers
    if (paired_parser_) {
        paired_parser_->close();
        paired_parser_.reset();
    }
    if (single_parser_) {
        single_parser_->close();
        single_parser_.reset();
    }

    // Clear queue
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        while (!ready_batches_.empty()) {
            ready_batches_.pop();
        }
    }

    Logger::instance().info("FASTQ worker threads stopped");
}

bool FastqWorker::get_next_batch(ReadBatch& batch) {
    std::unique_lock<std::mutex> lock(queue_mutex_);

    // Wait for a batch to be available
    queue_cv_.wait(lock, [this]() {
        return !ready_batches_.empty() || !running_ || stop_requested_;
    });

    if (stop_requested_ || (!running_ && ready_batches_.empty())) {
        return false;
    }

    if (ready_batches_.empty()) {
        return false;
    }

    // Get batch from queue
    batch = std::move(ready_batches_.front());
    ready_batches_.pop();

    // Notify workers that space is available
    space_cv_.notify_one();

    return true;
}

void FastqWorker::worker_thread() {
    while (running_ && !stop_requested_) {
        // Check if queue is full
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            space_cv_.wait(lock, [this]() {
                return ready_batches_.size() < static_cast<size_t>(max_prefetch_) ||
                       !running_ || stop_requested_;
            });

            if (stop_requested_ || !running_) {
                break;
            }
        }

        // Read a batch
        ReadBatch batch;
        read_batch_internal(batch);

        // Add to queue
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            if (batch.eof) {
                eof_ = true;
                // Push EOF marker
                ready_batches_.push(std::move(batch));
                queue_cv_.notify_one();
                break;
            }

            if (batch.count > 0) {
                ready_batches_.push(std::move(batch));
                queue_cv_.notify_one();
            }
        }
    }
}

void FastqWorker::read_batch_internal(ReadBatch& batch) {
    std::lock_guard<std::mutex> lock(parser_mutex_);

    batch.pairs.clear();
    batch.singles.clear();
    batch.count = 0;
    batch.eof = false;
    batch.is_paired = is_paired_;

    if (is_paired_ && paired_parser_) {
        batch.count = paired_parser_->next_batch(batch.pairs, batch_size_);
    } else if (!is_paired_ && single_parser_) {
        batch.count = single_parser_->next_batch(batch.singles, batch_size_);
    }

    if (batch.count == 0) {
        batch.eof = true;
    }
}

} // namespace winalign
