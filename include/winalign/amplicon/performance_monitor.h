#ifndef WINALIGN_AMPLICON_PERFORMANCE_MONITOR_H
#define WINALIGN_AMPLICON_PERFORMANCE_MONITOR_H

#include <string>
#include <memory>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace winalign {
namespace amplicon {

/**
 * Real-time performance monitoring and display
 * Shows live statistics, progress bars, GPU utilization, etc.
 */
class PerformanceMonitor {
public:
    PerformanceMonitor();
    ~PerformanceMonitor();

    /**
     * Start monitoring
     */
    void start();

    /**
     * Stop monitoring
     */
    void stop();

    /**
     * Update progress
     */
    void set_total_items(uint64_t total);
    void set_processed_items(uint64_t processed);
    void increment_processed(uint64_t count = 1);

    /**
     * Record timing for specific operations
     */
    void start_timer(const std::string& operation);
    void stop_timer(const std::string& operation);

    /**
     * Record throughput metrics
     */
    void record_throughput(const std::string& metric, double value);

    /**
     * Record memory usage
     */
    void record_memory_usage(size_t cpu_bytes, size_t gpu_bytes);

    /**
     * Record GPU utilization
     */
    void record_gpu_utilization(int gpu_id, float utilization);

    /**
     * Display real-time dashboard
     * Updates console with live stats
     */
    void display_dashboard();

    /**
     * Print final summary
     */
    void print_summary();

    /**
     * Get elapsed time
     */
    double get_elapsed_seconds() const;

    /**
     * Get estimated time remaining (ETA)
     */
    double get_eta_seconds() const;

    /**
     * Export metrics to JSON
     */
    std::string export_json() const;

    /**
     * Enable/disable live display
     */
    void set_live_display(bool enabled);

    /**
     * Set update interval for live display (ms)
     */
    void set_update_interval_ms(uint32_t interval);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * Scoped timer for automatic timing
 */
class ScopedTimer {
public:
    ScopedTimer(PerformanceMonitor& monitor, const std::string& operation)
        : monitor_(monitor), operation_(operation) {
        monitor_.start_timer(operation_);
    }

    ~ScopedTimer() {
        monitor_.stop_timer(operation_);
    }

private:
    PerformanceMonitor& monitor_;
    std::string operation_;
};

/**
 * Progress bar display
 */
class ProgressBar {
public:
    ProgressBar(uint64_t total, const std::string& description = "");
    ~ProgressBar();

    /**
     * Update progress
     */
    void update(uint64_t current);

    /**
     * Increment progress
     */
    void increment(uint64_t count = 1);

    /**
     * Mark as complete
     */
    void finish();

    /**
     * Set description
     */
    void set_description(const std::string& desc);

    /**
     * Set custom info text
     */
    void set_info(const std::string& info);

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * GPU utilization monitor
 * Periodically queries GPU usage
 */
class GPUMonitor {
public:
    GPUMonitor();
    ~GPUMonitor();

    /**
     * Start monitoring
     */
    void start(uint32_t interval_ms = 1000);

    /**
     * Stop monitoring
     */
    void stop();

    /**
     * Get current utilization for all GPUs
     */
    struct GPUStats {
        int device_id;
        float utilization;      // 0-100%
        float memory_used_mb;
        float memory_total_mb;
        float temperature_c;
        float power_watts;
    };

    std::vector<GPUStats> get_stats() const;

    /**
     * Get average utilization since start
     */
    float get_avg_utilization(int device_id) const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * System resource monitor
 */
class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();

    /**
     * Get CPU usage
     */
    float get_cpu_usage() const;

    /**
     * Get memory usage
     */
    struct MemoryStats {
        size_t total_mb;
        size_t used_mb;
        size_t available_mb;
        float usage_percent;
    };

    MemoryStats get_memory_stats() const;

    /**
     * Get disk I/O rate
     */
    struct DiskStats {
        float read_mb_per_sec;
        float write_mb_per_sec;
    };

    DiskStats get_disk_stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_PERFORMANCE_MONITOR_H
