#include "winalign/amplicon/realtime_processor.h"
#include <filesystem>
#include <thread>
#include <chrono>
#include <fstream>
#include <atomic>
#include <queue>
#include <mutex>

namespace fs = std::filesystem;

namespace winalign {
namespace amplicon {

/**
 * Incremental FASTQ reader implementation
 */
class IncrementalFASTQReader::Impl {
public:
    std::string filename_;
    std::ifstream file_;
    std::streampos last_position_;
    bool is_complete_;

    Impl(const std::string& filename)
        : filename_(filename), last_position_(0), is_complete_(false) {
        file_.open(filename_, std::ios::binary);
    }

    ~Impl() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    std::vector<Read> read_next_batch(size_t batch_size) {
        std::vector<Read> reads;

        if (!file_.is_open()) {
            return reads;
        }

        // Seek to last position
        file_.seekg(last_position_);

        while (reads.size() < batch_size && !file_.eof()) {
            std::string id_line, seq_line, plus_line, qual_line;

            if (!std::getline(file_, id_line)) break;
            if (!std::getline(file_, seq_line)) break;
            if (!std::getline(file_, plus_line)) break;
            if (!std::getline(file_, qual_line)) break;

            if (id_line[0] == '@' && plus_line[0] == '+') {
                Read read;
                read.sequence = seq_line;
                read.quality = qual_line;
                reads.push_back(read);
            }
        }

        // Save position for next read
        last_position_ = file_.tellg();

        return reads;
    }

    bool has_more_data() const {
        return file_.good() && !file_.eof();
    }

    bool is_complete() const {
        // Check if file is still being written
        // Simple heuristic: if file size hasn't changed for 5 seconds
        return is_complete_;
    }
};

IncrementalFASTQReader::IncrementalFASTQReader(const std::string& filename)
    : pimpl_(std::make_unique<Impl>(filename)) {}

IncrementalFASTQReader::~IncrementalFASTQReader() = default;

std::vector<Read> IncrementalFASTQReader::read_next_batch(size_t batch_size) {
    return pimpl_->read_next_batch(batch_size);
}

bool IncrementalFASTQReader::has_more_data() const {
    return pimpl_->has_more_data();
}

bool IncrementalFASTQReader::is_complete() const {
    return pimpl_->is_complete();
}

void IncrementalFASTQReader::reset() {
    pimpl_->last_position_ = 0;
    pimpl_->file_.clear();
    pimpl_->file_.seekg(0);
}

/**
 * Directory watcher implementation
 */
class DirectoryWatcher::Impl {
public:
    std::string directory_;
    std::unordered_map<std::string, std::filesystem::file_time_type> file_timestamps_;
    std::atomic<bool> running_;
    std::thread watch_thread_;
    FileEventCallback callback_;

    Impl(const std::string& directory)
        : directory_(directory), running_(false) {}

    ~Impl() {
        stop();
    }

    void start(FileEventCallback callback) {
        callback_ = callback;
        running_ = true;

        watch_thread_ = std::thread([this]() {
            while (running_) {
                check_for_changes();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
        });
    }

    void stop() {
        running_ = false;
        if (watch_thread_.joinable()) {
            watch_thread_.join();
        }
    }

    std::vector<std::string> check_for_changes() {
        std::vector<std::string> changed_files;

        if (!fs::exists(directory_)) {
            return changed_files;
        }

        // Scan directory for FASTQ files
        for (const auto& entry : fs::directory_iterator(directory_)) {
            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();
            std::string ext = entry.path().extension().string();

            // Only process FASTQ files
            if (ext != ".fastq" && ext != ".fq" && ext != ".fastq.gz") {
                continue;
            }

            auto last_write = fs::last_write_time(entry);
            auto it = file_timestamps_.find(path);

            if (it == file_timestamps_.end()) {
                // New file
                file_timestamps_[path] = last_write;
                changed_files.push_back(path);

                if (callback_) {
                    callback_(path, FileEvent::CREATED);
                }
            } else if (it->second != last_write) {
                // Modified file
                it->second = last_write;
                changed_files.push_back(path);

                if (callback_) {
                    callback_(path, FileEvent::MODIFIED);
                }
            }
        }

        return changed_files;
    }
};

DirectoryWatcher::DirectoryWatcher(const std::string& directory)
    : pimpl_(std::make_unique<Impl>(directory)) {}

DirectoryWatcher::~DirectoryWatcher() = default;

void DirectoryWatcher::start(FileEventCallback callback) {
    pimpl_->start(callback);
}

void DirectoryWatcher::stop() {
    pimpl_->stop();
}

std::vector<std::string> DirectoryWatcher::check_for_changes() {
    return pimpl_->check_for_changes();
}

/**
 * Real-time processor implementation
 */
class RealtimeProcessor::Impl {
public:
    RealtimeConfig rt_config_;
    AmpliconConfig amplicon_config_;
    std::atomic<bool> running_;
    std::thread processing_thread_;
    Stats stats_;
    CoverageStats coverage_stats_;

    FileEventCallback file_event_callback_;
    ProgressCallback progress_callback_;
    VariantCallback variant_callback_;

    std::unique_ptr<DirectoryWatcher> watcher_;
    std::queue<std::string> file_queue_;
    std::mutex queue_mutex_;

    Impl(const RealtimeConfig& rt_config, const AmpliconConfig& amplicon_config)
        : rt_config_(rt_config), amplicon_config_(amplicon_config), running_(false) {
        stats_ = Stats{0, 0, 0, 0, 0.0f,
                      std::chrono::system_clock::now(),
                      std::chrono::system_clock::now()};

        coverage_stats_ = CoverageStats{{}, 0, 0, 0.0f, false};
    }

    ~Impl() {
        stop();
    }

    void start() {
        if (running_) return;

        running_ = true;
        stats_.start_time = std::chrono::system_clock::now();

        // Start directory watcher
        watcher_ = std::make_unique<DirectoryWatcher>(rt_config_.watch_directory);
        watcher_->start([this](const std::string& file, FileEvent event) {
            handle_file_event(file, event);
        });

        // Start processing thread
        processing_thread_ = std::thread([this]() {
            process_loop();
        });
    }

    void stop() {
        running_ = false;

        if (watcher_) {
            watcher_->stop();
        }

        if (processing_thread_.joinable()) {
            processing_thread_.join();
        }
    }

    void handle_file_event(const std::string& file, FileEvent event) {
        if (file_event_callback_) {
            file_event_callback_(file, event);
        }

        if (event == FileEvent::CREATED || event == FileEvent::MODIFIED) {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            file_queue_.push(file);
        }
    }

    void process_loop() {
        while (running_) {
            std::string current_file;

            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                if (!file_queue_.empty()) {
                    current_file = file_queue_.front();
                    file_queue_.pop();
                }
            }

            if (!current_file.empty()) {
                process_file(current_file);
            } else {
                // No files to process, sleep briefly
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(rt_config_.poll_interval_ms)
                );
            }
        }
    }

    void process_file(const std::string& filename) {
        IncrementalFASTQReader reader(filename);
        uint64_t reads_in_file = 0;

        while (reader.has_more_data() || !reader.is_complete()) {
            auto reads = reader.read_next_batch(rt_config_.batch_size);

            if (reads.empty()) {
                // No new data yet, wait briefly
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            // Process this batch
            process_batch(reads);

            reads_in_file += reads.size();
            stats_.reads_processed += reads.size();

            // Update throughput
            auto now = std::chrono::system_clock::now();
            auto elapsed = std::chrono::duration<float>(now - stats_.start_time).count();
            stats_.current_throughput = stats_.reads_processed / elapsed;
            stats_.last_update = now;

            // Callback for progress
            if (progress_callback_) {
                progress_callback_(stats_.reads_processed, stats_.current_throughput);
            }

            // Check if we have sufficient coverage
            if (has_sufficient_coverage_internal(1000)) {
                coverage_stats_.sufficient_coverage = true;
            }
        }

        stats_.files_processed++;
    }

    void process_batch(const std::vector<Read>& reads) {
        // Simplified processing - in real implementation would call
        // full amplicon pipeline

        for (const auto& read : reads) {
            stats_.total_bases += read.sequence.length();
        }

        // TODO: Call actual processing pipeline
        // - Collapse reads
        // - Assign amplicons
        // - Align
        // - Call variants
        // - Update coverage stats

        // Placeholder: emit dummy variants
        // In real implementation, would emit actual variants via callback
    }

    bool has_sufficient_coverage_internal(uint32_t min_depth) const {
        if (coverage_stats_.amplicon_coverage.empty()) {
            return false;
        }

        for (const auto& [amplicon_id, coverage] : coverage_stats_.amplicon_coverage) {
            if (coverage < min_depth) {
                return false;
            }
        }

        return true;
    }
};

// Public interface

RealtimeProcessor::RealtimeProcessor(
    const RealtimeConfig& config,
    const AmpliconConfig& amplicon_config
) : pimpl_(std::make_unique<Impl>(config, amplicon_config)) {}

RealtimeProcessor::~RealtimeProcessor() = default;

void RealtimeProcessor::start() {
    pimpl_->start();
}

void RealtimeProcessor::stop() {
    pimpl_->stop();
}

bool RealtimeProcessor::is_running() const {
    return pimpl_->running_;
}

void RealtimeProcessor::set_file_event_callback(FileEventCallback callback) {
    pimpl_->file_event_callback_ = callback;
}

void RealtimeProcessor::set_progress_callback(ProgressCallback callback) {
    pimpl_->progress_callback_ = callback;
}

void RealtimeProcessor::set_variant_callback(VariantCallback callback) {
    pimpl_->variant_callback_ = callback;
}

RealtimeProcessor::Stats RealtimeProcessor::get_stats() const {
    return pimpl_->stats_;
}

RealtimeProcessor::CoverageStats RealtimeProcessor::get_coverage_stats() const {
    return pimpl_->coverage_stats_;
}

bool RealtimeProcessor::has_sufficient_coverage(uint32_t min_depth) const {
    return pimpl_->has_sufficient_coverage_internal(min_depth);
}

} // namespace amplicon
} // namespace winalign
