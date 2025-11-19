/**
 * @file pipeline_new_structure.cpp
 * @brief Simplified Pipeline implementation using extracted modules
 *
 * This file demonstrates the new structure for pipeline.cpp after integration.
 * The original 2,288-line implementation is reduced to ~350 lines by delegating
 * to extracted modules.
 *
 * BLUEPRINT FOR INTEGRATION - NOT YET ACTIVE
 */

#include "winalign/pipeline.h"
#include "pipeline_internal.h"
#include "pipeline_gpu_context.h"
#include "pipeline_metrics.h"
#include "pipeline_multistream.h"
#include "pipeline_batch_helpers.h"
#include "pipeline_initialization.h"
#include "winalign/fastq_parser.h"
#include "winalign/logger.h"
#include <atomic>
#include <memory>
#include <functional>

namespace winalign {

// PipelineConfig validation
bool PipelineConfig::is_valid() const {
    if (reference_fasta.empty()) return false;
    if (read1_fastq.empty()) return false;
    if (output_bam.empty()) return false;
    if (batch_size == 0 || batch_size > 100000) return false;
    if (kmer_size < 10 || kmer_size > 32) return false;
    return true;
}

// Simplified Pipeline implementation using extracted modules
class Pipeline::Impl {
public:
    explicit Impl(const PipelineConfig& cfg)
        : config_(cfg)
        , running_(false)
        , cancelled_(false)
        , mapping_quality_sum_(0.0)
    {
    }

    ~Impl() = default;

    Result<bool> initialize() {
        running_ = true;
        Logger::instance().info("Initializing pipeline with modular architecture");

        // Create and initialize the initialization module
        initializer_ = std::make_unique<internal::PipelineInitializer>(
            config_,
            [this](double p, const std::string& m) { update_progress(p, m); }
        );

        auto result = initializer_->initialize();
        if (!result.is_ok()) {
            running_ = false;
            return result;
        }

        // Create metrics collector
        metrics_ = std::make_unique<internal::MetricsCollector>();
        if (initializer_->is_gpu_enabled()) {
            if (!metrics_->initialize_cuda_events()) {
                Logger::instance().warn("Failed to initialize CUDA timing events");
            }
        }

        // Create batch processing helpers
        batch_helpers_ = std::make_unique<internal::BatchProcessingHelpers>(
            initializer_->get_reference_loader(),
            initializer_->get_bam_writer(),
            public_metrics_,
            mapping_quality_sum_
        );

        Logger::instance().info("Pipeline initialization complete (modular)");
        return Result<bool>(true);
    }

    Result<bool> run() {
        if (!running_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
        }

        Logger::instance().info("Starting alignment pipeline");
        update_progress(0.25, "Starting alignment");

        // Check if multi-stream GPU is available
        if (initializer_->is_gpu_enabled() &&
            initializer_->get_gpu_context_manager() &&
            initializer_->get_gpu_context_manager()->is_initialized()) {
            return run_multi_stream();
        }

        Logger::instance().info("Using single-stream pipeline (GPU not available)");
        return run_single_stream();
    }

    Result<bool> run_multi_stream() {
        if (!running_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
        }

        Logger::instance().info("Using multi-stream GPU scheduler");

        // Create multi-stream scheduler
        scheduler_ = std::make_unique<internal::MultiStreamScheduler>(
            config_,
            initializer_->get_gpu_context_manager()->get_contexts(),
            *metrics_,
            cancelled_
        );

        // Initialize scheduler with all required resources
        uint64_t estimated_reads = estimate_total_reads();
        scheduler_->initialize(
            initializer_->get_fm_index_view(),
            initializer_->get_device_reference(),
            initializer_->get_reference_length(),
            initializer_->get_bam_writer(),
            estimated_reads,
            [this](double p, const std::string& m) { update_progress(p, m); }
        );

        // Run the multi-stream scheduler
        auto result = scheduler_->run();

        return result;
    }

    Result<bool> run_single_stream() {
        // Fallback for CPU-only or single-stream execution
        // This would contain simplified single-stream logic
        // For now, return success (full implementation would go here)

        Logger::instance().warn("Single-stream implementation not yet available in modular version");
        update_progress(0.90, "Alignment complete (single-stream)");

        return Result<bool>(true);
    }

    Result<bool> finalize() {
        update_progress(0.90, "Finalizing output");

        // Log performance summary via metrics module
        if (metrics_) {
            metrics_->log_performance_summary();
        }

        // Update final metrics
        public_metrics_.total_time_seconds =
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start_time_
            ).count();

        update_progress(1.0, "Complete");
        running_ = false;

        return Result<bool>(true);
    }

    void cancel() {
        cancelled_ = true;
        Logger::instance().info("Pipeline cancellation requested");
    }

    void set_progress_callback(ProgressCallback callback) {
        progress_callback_ = callback;
    }

    const PipelineMetrics& get_metrics() const {
        return public_metrics_;
    }

private:
    void update_progress(double progress, const std::string& message) {
        if (progress_callback_) {
            progress_callback_(progress, message);
        }
    }

    uint64_t estimate_total_reads() {
        // Estimate total reads by parsing FASTQ headers
        // Simple implementation - could be improved
        uint64_t estimate = 100000; // Default estimate

        try {
            auto parser = std::make_unique<FastqParser>(config_.read1_fastq);
            auto result = parser->open();
            if (result.is_ok()) {
                // Quick estimate based on file size
                estimate = config_.batch_size * 10; // Rough estimate
                parser->close();
            }
        } catch (...) {
            // Use default estimate if parsing fails
        }

        return estimate;
    }

    // Configuration
    const PipelineConfig& config_;

    // Module instances (owned)
    std::unique_ptr<internal::PipelineInitializer> initializer_;
    std::unique_ptr<internal::MultiStreamScheduler> scheduler_;
    std::unique_ptr<internal::BatchProcessingHelpers> batch_helpers_;
    std::unique_ptr<internal::MetricsCollector> metrics_;

    // State
    std::atomic<bool> running_;
    std::atomic<bool> cancelled_;
    ProgressCallback progress_callback_;
    std::chrono::steady_clock::time_point start_time_;

    // Metrics (for public API)
    PipelineMetrics public_metrics_;
    double mapping_quality_sum_;
};

// Pipeline public interface (PIMPL wrapper)
Pipeline::Pipeline(const PipelineConfig& config)
    : pimpl_(std::make_unique<Impl>(config)) {}

Pipeline::~Pipeline() = default;

Result<bool> Pipeline::initialize() {
    return pimpl_->initialize();
}

Result<bool> Pipeline::run() {
    auto result = pimpl_->run();
    if (result.is_ok()) {
        return pimpl_->finalize();
    }
    return result;
}

Result<bool> Pipeline::finalize() {
    return pimpl_->finalize();
}

void Pipeline::cancel() {
    pimpl_->cancel();
}

void Pipeline::set_progress_callback(ProgressCallback callback) {
    pimpl_->set_progress_callback(callback);
}

const PipelineMetrics& Pipeline::get_metrics() const {
    return pimpl_->get_metrics();
}

} // namespace winalign

/*
 * SUMMARY OF CHANGES
 * ==================
 *
 * This file demonstrates the new pipeline architecture:
 *
 * 1. **Reduced Complexity**: ~250 lines vs 2,288 original
 *    - 89% reduction in code size
 *    - Clear, focused implementation
 *
 * 2. **Module Delegation**:
 *    - PipelineInitializer: All initialization (reference, GPU, BAM)
 *    - MultiStreamScheduler: Multi-stream execution
 *    - BatchProcessingHelpers: Alignment building
 *    - MetricsCollector: Performance tracking
 *    - GpuContextManager: GPU context lifecycle (via initializer)
 *
 * 3. **Benefits**:
 *    - Easier to understand and maintain
 *    - Independently testable modules
 *    - Clear separation of concerns
 *    - Reduced cognitive load
 *
 * 4. **Integration Steps**:
 *    - Replace current pipeline.cpp with this structure
 *    - Implement run_single_stream() for CPU fallback
 *    - Add any missing helper methods
 *    - Test thoroughly
 *
 * 5. **Line Count**:
 *    - Pipeline::Impl class: ~150 lines
 *    - Helper methods: ~50 lines
 *    - PIMPL wrapper: ~50 lines
 *    - Total: ~250 lines (vs 2,288 before)
 */
