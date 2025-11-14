#include "winalign/pipeline.h"
#include <iostream>

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

// Pipeline implementation (PIMPL pattern)
class Pipeline::Impl {
public:
    Impl(const PipelineConfig& cfg) : config_(cfg) {}

    Result<bool> initialize() {
        // TODO: Load reference, build index, allocate GPU memory
        std::cout << "Initializing pipeline (stub implementation)\n";
        return Result<bool>(true);
    }

    Result<bool> run() {
        // TODO: Main alignment loop
        std::cout << "Running alignment pipeline (stub implementation)\n";
        return Result<bool>(true);
    }

    Result<bool> finalize() {
        // TODO: Write final output, generate metrics
        std::cout << "Finalizing pipeline (stub implementation)\n";
        return Result<bool>(true);
    }

    const QCMetrics& get_metrics() const {
        return metrics_;
    }

    void set_progress_callback(std::function<void(double)> callback) {
        progress_callback_ = std::move(callback);
    }

    void cancel() {
        cancelled_ = true;
    }

    bool is_running() const {
        return running_;
    }

private:
    PipelineConfig config_;
    QCMetrics metrics_;
    std::function<void(double)> progress_callback_;
    bool running_ = false;
    bool cancelled_ = false;
};

// Pipeline public interface
Pipeline::Pipeline(const PipelineConfig& config)
    : pimpl_(std::make_unique<Impl>(config)) {}

Pipeline::~Pipeline() = default;

Result<bool> Pipeline::initialize() {
    return pimpl_->initialize();
}

Result<bool> Pipeline::run() {
    return pimpl_->run();
}

Result<bool> Pipeline::finalize() {
    return pimpl_->finalize();
}

const QCMetrics& Pipeline::get_metrics() const {
    return pimpl_->get_metrics();
}

void Pipeline::set_progress_callback(std::function<void(double)> callback) {
    pimpl_->set_progress_callback(std::move(callback));
}

void Pipeline::cancel() {
    pimpl_->cancel();
}

bool Pipeline::is_running() const {
    return pimpl_->is_running();
}

} // namespace winalign
