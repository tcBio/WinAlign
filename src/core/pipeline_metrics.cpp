/**
 * @file pipeline_metrics.cpp
 * @brief Performance metrics collection and reporting implementation
 *
 * Tracks timing and performance counters throughout pipeline execution,
 * including CUDA event-based GPU timing.
 */

#include "pipeline_metrics.h"
#include "winalign/logger.h"
#include <string>

namespace winalign {
namespace internal {

MetricsCollector::MetricsCollector()
    : batch_count_(0)
    , cuda_events_initialized_(false)
{
    // Initialize cumulative timing to zeros
    cumulative_timing_ = BatchTiming{};
    perf_counters_ = PerformanceCounters{};
}

MetricsCollector::~MetricsCollector() {
    cleanup_cuda_events();
}

bool MetricsCollector::initialize_cuda_events() {
    if (cuda_events_initialized_) {
        Logger::instance().warn("CUDA timing events already initialized");
        return true;
    }

    // Create all timing events
    cudaError_t err = cudaSuccess;

    err = cudaEventCreate(&event_h2d_start_);
    if (err != cudaSuccess) goto cleanup;

    err = cudaEventCreate(&event_h2d_done_);
    if (err != cudaSuccess) goto cleanup;

    err = cudaEventCreate(&event_seeding_start_);
    if (err != cudaSuccess) goto cleanup;

    err = cudaEventCreate(&event_seeding_done_);
    if (err != cudaSuccess) goto cleanup;

    err = cudaEventCreate(&event_sw_start_);
    if (err != cudaSuccess) goto cleanup;

    err = cudaEventCreate(&event_sw_done_);
    if (err != cudaSuccess) goto cleanup;

    err = cudaEventCreate(&event_d2h_start_);
    if (err != cudaSuccess) goto cleanup;

    err = cudaEventCreate(&event_d2h_done_);
    if (err != cudaSuccess) goto cleanup;

    cuda_events_initialized_ = true;
    Logger::instance().info("CUDA timing events initialized");
    return true;

cleanup:
    Logger::instance().error(
        "Failed to create CUDA timing events: " +
        std::string(cudaGetErrorString(err)));
    cleanup_cuda_events();
    return false;
}

void MetricsCollector::cleanup_cuda_events() {
    if (event_h2d_start_) {
        cudaEventDestroy(event_h2d_start_);
        event_h2d_start_ = nullptr;
    }
    if (event_h2d_done_) {
        cudaEventDestroy(event_h2d_done_);
        event_h2d_done_ = nullptr;
    }
    if (event_seeding_start_) {
        cudaEventDestroy(event_seeding_start_);
        event_seeding_start_ = nullptr;
    }
    if (event_seeding_done_) {
        cudaEventDestroy(event_seeding_done_);
        event_seeding_done_ = nullptr;
    }
    if (event_sw_start_) {
        cudaEventDestroy(event_sw_start_);
        event_sw_start_ = nullptr;
    }
    if (event_sw_done_) {
        cudaEventDestroy(event_sw_done_);
        event_sw_done_ = nullptr;
    }
    if (event_d2h_start_) {
        cudaEventDestroy(event_d2h_start_);
        event_d2h_start_ = nullptr;
    }
    if (event_d2h_done_) {
        cudaEventDestroy(event_d2h_done_);
        event_d2h_done_ = nullptr;
    }

    cuda_events_initialized_ = false;
}

void MetricsCollector::record_batch(const BatchTiming& timing, uint32_t batch_num) {
    // Add to cumulative totals
    cumulative_timing_.t_read += timing.t_read;
    cumulative_timing_.t_h2d += timing.t_h2d;
    cumulative_timing_.t_gpu_seed += timing.t_gpu_seed;
    cumulative_timing_.t_gpu_align += timing.t_gpu_align;
    cumulative_timing_.t_d2h += timing.t_d2h;
    cumulative_timing_.t_write += timing.t_write;
    cumulative_timing_.t_total += timing.t_total;

    batch_count_++;

    // Log this batch's timing
    log_batch_timing(timing, batch_num);
}

void MetricsCollector::update_counters(const PerformanceCounters& counters) {
    perf_counters_.gpu_aligned_reads += counters.gpu_aligned_reads;
    perf_counters_.unmapped_reads += counters.unmapped_reads;
    perf_counters_.reads_without_seeds += counters.reads_without_seeds;
    perf_counters_.total_gpu_seeds += counters.total_gpu_seeds;
    perf_counters_.batches_processed += counters.batches_processed;
}

float MetricsCollector::get_cuda_event_time(cudaEvent_t start, cudaEvent_t stop) const {
    float elapsed_ms = 0.0f;
    cudaEventElapsedTime(&elapsed_ms, start, stop);
    return elapsed_ms;
}

void MetricsCollector::log_batch_timing(const BatchTiming& timing, uint32_t batch_num) const {
    Logger::instance().info(
        "Batch " + std::to_string(batch_num) + " timing: " +
        "read=" + std::to_string(timing.t_read) + "ms, " +
        "H2D=" + std::to_string(timing.t_h2d) + "ms, " +
        "seed=" + std::to_string(timing.t_gpu_seed) + "ms, " +
        "align=" + std::to_string(timing.t_gpu_align) + "ms, " +
        "D2H=" + std::to_string(timing.t_d2h) + "ms, " +
        "write=" + std::to_string(timing.t_write) + "ms, " +
        "total=" + std::to_string(timing.t_total) + "ms"
    );
}

void MetricsCollector::log_performance_summary() const {
    if (batch_count_ == 0) {
        Logger::instance().info("No batches processed - no performance summary available");
        return;
    }

    Logger::instance().info("=== Performance Summary ===");
    Logger::instance().info("Total batches: " + std::to_string(batch_count_));
    Logger::instance().info("GPU aligned reads: " + std::to_string(perf_counters_.gpu_aligned_reads));
    Logger::instance().info("Unmapped reads: " + std::to_string(perf_counters_.unmapped_reads));
    Logger::instance().info("Reads without seeds: " + std::to_string(perf_counters_.reads_without_seeds));
    Logger::instance().info("Total GPU seeds: " + std::to_string(perf_counters_.total_gpu_seeds));
    Logger::instance().info("Avg seeds/read: " + std::to_string(perf_counters_.avg_seeds_per_read()));

    Logger::instance().info("=== Cumulative Timing (avg per batch) ===");
    Logger::instance().info("Read: " + std::to_string(cumulative_timing_.t_read / batch_count_) + " ms");
    Logger::instance().info("H2D: " + std::to_string(cumulative_timing_.t_h2d / batch_count_) + " ms");
    Logger::instance().info("GPU Seed: " + std::to_string(cumulative_timing_.t_gpu_seed / batch_count_) + " ms");
    Logger::instance().info("GPU Align: " + std::to_string(cumulative_timing_.t_gpu_align / batch_count_) + " ms");
    Logger::instance().info("D2H: " + std::to_string(cumulative_timing_.t_d2h / batch_count_) + " ms");
    Logger::instance().info("Write: " + std::to_string(cumulative_timing_.t_write / batch_count_) + " ms");
    Logger::instance().info("Total: " + std::to_string(cumulative_timing_.t_total / batch_count_) + " ms");
}

const BatchTiming& MetricsCollector::get_cumulative_timing() const {
    return cumulative_timing_;
}

const PerformanceCounters& MetricsCollector::get_counters() const {
    return perf_counters_;
}

uint32_t MetricsCollector::get_batch_count() const {
    return batch_count_;
}

} // namespace internal
} // namespace winalign
