#include "winalign/amplicon/performance_monitor.h"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>

#ifdef __linux__
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace winalign {
namespace amplicon {

/**
 * Progress bar implementation
 */
class ProgressBar::Impl {
public:
    uint64_t total_;
    std::atomic<uint64_t> current_;
    std::string description_;
    std::string info_;
    std::chrono::steady_clock::time_point start_time_;
    std::mutex mutex_;
    bool finished_;

    Impl(uint64_t total, const std::string& desc)
        : total_(total), current_(0), description_(desc),
          start_time_(std::chrono::steady_clock::now()),
          finished_(false) {}

    void display() {
        std::lock_guard<std::mutex> lock(mutex_);

        if (finished_) return;

        uint64_t curr = current_.load();
        float progress = total_ > 0 ? static_cast<float>(curr) / total_ : 0.0f;

        // Calculate ETA
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - start_time_).count();
        double eta = (progress > 0.01) ? (elapsed / progress) - elapsed : 0.0;

        // Progress bar visualization
        const int bar_width = 50;
        int pos = static_cast<int>(bar_width * progress);

        std::cout << "\r[";
        for (int i = 0; i < bar_width; ++i) {
            if (i < pos) std::cout << "=";
            else if (i == pos) std::cout << ">";
            else std::cout << " ";
        }

        std::cout << "] " << std::fixed << std::setprecision(1)
                  << (progress * 100.0) << "% ";

        if (!description_.empty()) {
            std::cout << description_ << " ";
        }

        std::cout << curr << "/" << total_;

        // ETA
        if (eta > 0 && !finished_) {
            int eta_min = static_cast<int>(eta / 60);
            int eta_sec = static_cast<int>(eta) % 60;
            std::cout << " ETA: " << eta_min << "m" << eta_sec << "s";
        }

        // Custom info
        if (!info_.empty()) {
            std::cout << " | " << info_;
        }

        std::cout << std::flush;
    }
};

ProgressBar::ProgressBar(uint64_t total, const std::string& description)
    : pimpl_(std::make_unique<Impl>(total, description)) {}

ProgressBar::~ProgressBar() {
    finish();
}

void ProgressBar::update(uint64_t current) {
    pimpl_->current_ = current;
    pimpl_->display();
}

void ProgressBar::increment(uint64_t count) {
    pimpl_->current_ += count;
    pimpl_->display();
}

void ProgressBar::finish() {
    if (!pimpl_->finished_) {
        pimpl_->current_ = pimpl_->total_;
        pimpl_->finished_ = true;
        pimpl_->display();
        std::cout << "\n";
    }
}

void ProgressBar::set_description(const std::string& desc) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->description_ = desc;
}

void ProgressBar::set_info(const std::string& info) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->info_ = info;
}

/**
 * Performance monitor implementation
 */
class PerformanceMonitor::Impl {
public:
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> total_items_;
    std::atomic<uint64_t> processed_items_;

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> timer_starts_;
    std::unordered_map<std::string, double> timer_totals_;
    std::unordered_map<std::string, uint64_t> timer_counts_;

    std::unordered_map<std::string, double> throughput_metrics_;

    std::atomic<size_t> cpu_memory_bytes_;
    std::atomic<size_t> gpu_memory_bytes_;

    std::unordered_map<int, std::vector<float>> gpu_utilization_history_;

    bool live_display_enabled_;
    uint32_t update_interval_ms_;
    std::thread display_thread_;
    std::atomic<bool> running_;

    std::mutex mutex_;

    Impl()
        : total_items_(0), processed_items_(0),
          cpu_memory_bytes_(0), gpu_memory_bytes_(0),
          live_display_enabled_(false), update_interval_ms_(1000),
          running_(false) {
        start_time_ = std::chrono::steady_clock::now();
    }

    ~Impl() {
        stop();
    }

    void start() {
        if (running_) return;

        running_ = true;
        start_time_ = std::chrono::steady_clock::now();

        if (live_display_enabled_) {
            display_thread_ = std::thread([this]() {
                while (running_) {
                    display_dashboard();
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(update_interval_ms_)
                    );
                }
            });
        }
    }

    void stop() {
        running_ = false;
        if (display_thread_.joinable()) {
            display_thread_.join();
        }
    }

    double get_elapsed_seconds() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - start_time_).count();
    }

    double get_eta_seconds() const {
        uint64_t total = total_items_.load();
        uint64_t processed = processed_items_.load();

        if (total == 0 || processed == 0) return 0.0;

        double elapsed = get_elapsed_seconds();
        double progress = static_cast<double>(processed) / total;

        if (progress < 0.01) return 0.0;

        return (elapsed / progress) - elapsed;
    }

    void display_dashboard() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Clear screen (ANSI escape code)
        std::cout << "\033[2J\033[1;1H";

        std::cout << "=== WinAlign-Amplicon Performance Dashboard ===\n\n";

        // Progress
        uint64_t total = total_items_.load();
        uint64_t processed = processed_items_.load();
        float progress = total > 0 ? static_cast<float>(processed) / total * 100.0f : 0.0f;

        std::cout << "Progress: " << processed << " / " << total
                  << " (" << std::fixed << std::setprecision(1) << progress << "%)\n";

        // Timing
        double elapsed = get_elapsed_seconds();
        double eta = get_eta_seconds();

        std::cout << "Elapsed: " << format_time(elapsed) << "\n";
        if (eta > 0) {
            std::cout << "ETA: " << format_time(eta) << "\n";
        }

        // Throughput
        if (elapsed > 0 && processed > 0) {
            double throughput = processed / elapsed;
            std::cout << "Throughput: " << std::fixed << std::setprecision(0)
                      << throughput << " items/sec\n";
        }

        std::cout << "\n";

        // Stage timings
        if (!timer_totals_.empty()) {
            std::cout << "Stage Timings:\n";
            for (const auto& [stage, total_time] : timer_totals_) {
                uint64_t count = timer_counts_[stage];
                double avg_time = count > 0 ? total_time / count : 0.0;

                std::cout << "  " << std::setw(25) << std::left << stage << ": "
                          << std::fixed << std::setprecision(2) << avg_time << " ms/item"
                          << " (" << count << " calls)\n";
            }
            std::cout << "\n";
        }

        // Memory
        size_t cpu_mb = cpu_memory_bytes_.load() / 1024 / 1024;
        size_t gpu_mb = gpu_memory_bytes_.load() / 1024 / 1024;

        std::cout << "Memory Usage:\n";
        std::cout << "  CPU: " << cpu_mb << " MB\n";
        std::cout << "  GPU: " << gpu_mb << " MB\n";
        std::cout << "\n";

        // GPU utilization
        if (!gpu_utilization_history_.empty()) {
            std::cout << "GPU Utilization:\n";
            for (const auto& [gpu_id, history] : gpu_utilization_history_) {
                if (!history.empty()) {
                    float avg = 0.0f;
                    for (float util : history) {
                        avg += util;
                    }
                    avg /= history.size();

                    std::cout << "  GPU " << gpu_id << ": "
                              << std::fixed << std::setprecision(1) << avg << "%\n";
                }
            }
        }
    }

    void print_summary() {
        std::lock_guard<std::mutex> lock(mutex_);

        std::cout << "\n=== Processing Summary ===\n\n";

        uint64_t total = total_items_.load();
        uint64_t processed = processed_items_.load();
        double elapsed = get_elapsed_seconds();

        std::cout << "Total items: " << total << "\n";
        std::cout << "Processed: " << processed << "\n";
        std::cout << "Time: " << format_time(elapsed) << "\n";

        if (elapsed > 0 && processed > 0) {
            double throughput = processed / elapsed;
            std::cout << "Throughput: " << std::fixed << std::setprecision(2)
                      << throughput << " items/sec\n";
        }

        std::cout << "\n";

        // Detailed timings
        if (!timer_totals_.empty()) {
            std::cout << "Detailed Timings:\n";
            for (const auto& [stage, total_time] : timer_totals_) {
                uint64_t count = timer_counts_[stage];
                double avg_time = count > 0 ? total_time / count : 0.0;
                double percent = elapsed > 0 ? (total_time / (elapsed * 1000.0)) * 100.0 : 0.0;

                std::cout << "  " << std::setw(30) << std::left << stage << ": "
                          << std::setw(8) << std::right << std::fixed << std::setprecision(2)
                          << avg_time << " ms/item"
                          << " (" << std::setw(5) << std::setprecision(1) << percent << "%)\n";
            }
        }

        std::cout << "\n";
    }

private:
    std::string format_time(double seconds) const {
        int hours = static_cast<int>(seconds / 3600);
        int minutes = static_cast<int>((seconds - hours * 3600) / 60);
        int secs = static_cast<int>(seconds) % 60;

        std::ostringstream oss;
        if (hours > 0) {
            oss << hours << "h " << minutes << "m " << secs << "s";
        } else if (minutes > 0) {
            oss << minutes << "m " << secs << "s";
        } else {
            oss << std::fixed << std::setprecision(1) << seconds << "s";
        }
        return oss.str();
    }
};

// Public interface

PerformanceMonitor::PerformanceMonitor()
    : pimpl_(std::make_unique<Impl>()) {}

PerformanceMonitor::~PerformanceMonitor() = default;

void PerformanceMonitor::start() {
    pimpl_->start();
}

void PerformanceMonitor::stop() {
    pimpl_->stop();
}

void PerformanceMonitor::set_total_items(uint64_t total) {
    pimpl_->total_items_ = total;
}

void PerformanceMonitor::set_processed_items(uint64_t processed) {
    pimpl_->processed_items_ = processed;
}

void PerformanceMonitor::increment_processed(uint64_t count) {
    pimpl_->processed_items_ += count;
}

void PerformanceMonitor::start_timer(const std::string& operation) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->timer_starts_[operation] = std::chrono::steady_clock::now();
}

void PerformanceMonitor::stop_timer(const std::string& operation) {
    auto end_time = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(pimpl_->mutex_);

    auto it = pimpl_->timer_starts_.find(operation);
    if (it != pimpl_->timer_starts_.end()) {
        auto duration_ms = std::chrono::duration<double, std::milli>(
            end_time - it->second
        ).count();

        pimpl_->timer_totals_[operation] += duration_ms;
        pimpl_->timer_counts_[operation]++;

        pimpl_->timer_starts_.erase(it);
    }
}

void PerformanceMonitor::record_throughput(const std::string& metric, double value) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->throughput_metrics_[metric] = value;
}

void PerformanceMonitor::record_memory_usage(size_t cpu_bytes, size_t gpu_bytes) {
    pimpl_->cpu_memory_bytes_ = cpu_bytes;
    pimpl_->gpu_memory_bytes_ = gpu_bytes;
}

void PerformanceMonitor::record_gpu_utilization(int gpu_id, float utilization) {
    std::lock_guard<std::mutex> lock(pimpl_->mutex_);
    pimpl_->gpu_utilization_history_[gpu_id].push_back(utilization);
}

void PerformanceMonitor::display_dashboard() {
    pimpl_->display_dashboard();
}

void PerformanceMonitor::print_summary() {
    pimpl_->print_summary();
}

double PerformanceMonitor::get_elapsed_seconds() const {
    return pimpl_->get_elapsed_seconds();
}

double PerformanceMonitor::get_eta_seconds() const {
    return pimpl_->get_eta_seconds();
}

void PerformanceMonitor::set_live_display(bool enabled) {
    pimpl_->live_display_enabled_ = enabled;
}

void PerformanceMonitor::set_update_interval_ms(uint32_t interval) {
    pimpl_->update_interval_ms_ = interval;
}

std::string PerformanceMonitor::export_json() const {
    // Simple JSON export
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"elapsed_seconds\": " << pimpl_->get_elapsed_seconds() << ",\n";
    oss << "  \"total_items\": " << pimpl_->total_items_.load() << ",\n";
    oss << "  \"processed_items\": " << pimpl_->processed_items_.load() << "\n";
    oss << "}\n";
    return oss.str();
}

} // namespace amplicon
} // namespace winalign
