/**
 * @file pipeline_metrics.h
 * @brief Performance metrics collection and reporting for pipeline
 */

#pragma once

#include "pipeline_internal.h"
#include <cuda_runtime.h>
#include <cstdint>

namespace winalign {
namespace internal {

/**
 * @brief Collects and reports performance metrics for the pipeline
 *
 * This class tracks timing information and performance counters throughout
 * the pipeline execution, including GPU timing via CUDA events.
 */
class MetricsCollector {
public:
    /**
     * @brief Construct metrics collector
     */
    MetricsCollector();

    /**
     * @brief Destructor - ensures CUDA events are cleaned up
     */
    ~MetricsCollector();

    /**
     * @brief Initialize CUDA timing events
     * @return true if initialization succeeded, false otherwise
     */
    bool initialize_cuda_events();

    /**
     * @brief Clean up CUDA timing events
     */
    void cleanup_cuda_events();

    /**
     * @brief Record timing for a completed batch
     * @param timing Timing data for the batch
     * @param batch_num Batch number (for logging)
     */
    void record_batch(const BatchTiming& timing, uint32_t batch_num);

    /**
     * @brief Update performance counters
     * @param counters Performance counters to add to cumulative totals
     */
    void update_counters(const PerformanceCounters& counters);

    /**
     * @brief Get elapsed time between two CUDA events
     * @param start Start event
     * @param stop Stop event
     * @return Elapsed time in milliseconds
     */
    float get_cuda_event_time(cudaEvent_t start, cudaEvent_t stop) const;

    /**
     * @brief Log complete performance summary
     */
    void log_performance_summary() const;

    /**
     * @brief Get cumulative timing data
     * @return Reference to cumulative timing
     */
    const BatchTiming& get_cumulative_timing() const;

    /**
     * @brief Get performance counters
     * @return Reference to performance counters
     */
    const PerformanceCounters& get_counters() const;

    /**
     * @brief Get total number of batches processed
     * @return Batch count
     */
    uint32_t get_batch_count() const;

    // CUDA timing events (public for pipeline use)
    cudaEvent_t event_h2d_start_ = nullptr;
    cudaEvent_t event_h2d_done_ = nullptr;
    cudaEvent_t event_seeding_start_ = nullptr;
    cudaEvent_t event_seeding_done_ = nullptr;
    cudaEvent_t event_sw_start_ = nullptr;
    cudaEvent_t event_sw_done_ = nullptr;
    cudaEvent_t event_d2h_start_ = nullptr;
    cudaEvent_t event_d2h_done_ = nullptr;

private:
    /**
     * @brief Log timing for a single batch
     * @param timing Batch timing data
     * @param batch_num Batch number
     */
    void log_batch_timing(const BatchTiming& timing, uint32_t batch_num) const;

    // Cumulative statistics
    BatchTiming cumulative_timing_;
    PerformanceCounters perf_counters_;
    uint32_t batch_count_;
    bool cuda_events_initialized_;
};

} // namespace internal
} // namespace winalign
