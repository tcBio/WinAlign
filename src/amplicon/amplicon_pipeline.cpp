#include "winalign/amplicon/amplicon_pipeline.h"
#include "winalign/amplicon/read_collapser.h"
#include "winalign/amplicon/amplicon_assigner.h"
#include "winalign/amplicon/variant_caller.h"
#include "winalign/fastq_parser.h"
#include "winalign/reference_loader.h"
#include <iostream>

namespace winalign {
namespace amplicon {

// PIMPL implementation
class AmpliconPipeline::Impl {
public:
    Impl(const AmpliconConfig& cfg) : config_(cfg) {}

    Result<bool> initialize() {
        running_ = true;

        // Stage 1: Load reference genome
        update_progress(0.0, "Loading reference genome");
        reference_loader_ = std::make_unique<ReferenceLoader>(config_.reference_fasta);
        auto load_result = reference_loader_->load();
        if (!load_result.is_ok()) {
            running_ = false;
            return load_result;
        }

        update_progress(0.1, "Reference loaded");

        // Stage 2: Initialize components
        update_progress(0.1, "Initializing amplicon assigner");
        assigner_ = std::make_unique<AmpliconAssigner>(config_.targets);

        update_progress(0.15, "Initializing read collapser");
        collapser_ = std::make_unique<ReadCollapser>(config_.max_cluster_distance);

        update_progress(0.2, "Initializing variant caller");
        variant_caller_ = std::make_unique<VariantCaller>(config_);

        update_progress(0.25, "Initialization complete");

        return Result<bool>(true);
    }

    Result<bool> run() {
        if (!running_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
        }

        // Stage 3: Process FASTQ (0.25 - 0.90)
        update_progress(0.25, "Opening FASTQ file");

        FastqParser parser(config_.input_fastq);
        auto open_result = parser.open();
        if (!open_result.is_ok()) {
            running_ = false;
            return open_result;
        }

        // Process in batches
        std::vector<Read> read_batch;
        uint64_t total_reads = 0;
        uint64_t total_clusters = 0;

        while (!cancelled_) {
            // Read batch
            read_batch.clear();
            size_t batch_count = parser.next_batch(read_batch, config_.batch_size);

            if (batch_count == 0) break; // EOF

            total_reads += batch_count;
            double progress = 0.25 + (0.65 * total_reads / 1000000.0); // Estimate
            progress = std::min(progress, 0.90);

            update_progress(progress,
                          "Processing reads: " + std::to_string(total_reads));

            // Stage 3a: Collapse reads
            auto clusters = collapser_->collapse(read_batch);
            total_clusters += clusters.size();

            std::cout << "Collapsed " << batch_count << " reads into "
                      << clusters.size() << " clusters ("
                      << (float)batch_count / clusters.size() << "x reduction)\n";

            // Stage 3b: Assign amplicons
            assigner_->assign_batch(clusters);

            // TODO: Stage 3c: Align unique sequences
            // TODO: Stage 3d: Call variants
            // TODO: Stage 3e: Write to VCF

            stats_.total_reads = total_reads;
            stats_.collapsed_clusters = total_clusters;
        }

        parser.close();
        update_progress(0.90, "Processing complete");

        return Result<bool>(true);
    }

    Result<bool> finalize() {
        update_progress(0.90, "Finalizing");

        // Write statistics
        if (!config_.output_stats.empty()) {
            write_statistics();
        }

        update_progress(1.0, "Complete");
        running_ = false;

        return Result<bool>(true);
    }

    PipelineStats get_stats() const {
        return stats_;
    }

    void set_progress_callback(std::function<void(double, const std::string&)> callback) {
        progress_callback_ = std::move(callback);
    }

    void cancel() {
        cancelled_ = true;
    }

    bool is_running() const {
        return running_;
    }

private:
    void update_progress(double progress, const std::string& message) {
        current_progress_ = progress;
        if (progress_callback_) {
            progress_callback_(progress, message);
        }
    }

    void write_statistics() {
        // TODO: Write JSON statistics file
        std::cout << "\nPipeline Statistics:\n";
        std::cout << "  Total reads: " << stats_.total_reads << "\n";
        std::cout << "  Collapsed clusters: " << stats_.collapsed_clusters << "\n";
        std::cout << "  Collapse ratio: " << stats_.collapse_ratio << "x\n";
    }

    AmpliconConfig config_;
    PipelineStats stats_;
    bool running_ = false;
    bool cancelled_ = false;
    double current_progress_ = 0.0;

    std::function<void(double, const std::string&)> progress_callback_;

    // Components
    std::unique_ptr<ReferenceLoader> reference_loader_;
    std::unique_ptr<ReadCollapser> collapser_;
    std::unique_ptr<AmpliconAssigner> assigner_;
    std::unique_ptr<VariantCaller> variant_caller_;
};

// Public interface
AmpliconPipeline::AmpliconPipeline(const AmpliconConfig& config)
    : pimpl_(std::make_unique<Impl>(config)) {}

AmpliconPipeline::~AmpliconPipeline() = default;

Result<bool> AmpliconPipeline::initialize() {
    return pimpl_->initialize();
}

Result<bool> AmpliconPipeline::run() {
    return pimpl_->run();
}

Result<bool> AmpliconPipeline::finalize() {
    return pimpl_->finalize();
}

AmpliconPipeline::PipelineStats AmpliconPipeline::get_stats() const {
    return pimpl_->get_stats();
}

void AmpliconPipeline::set_progress_callback(
    std::function<void(double, const std::string&)> callback) {
    pimpl_->set_progress_callback(std::move(callback));
}

void AmpliconPipeline::cancel() {
    pimpl_->cancel();
}

bool AmpliconPipeline::is_running() const {
    return pimpl_->is_running();
}

} // namespace amplicon
} // namespace winalign
