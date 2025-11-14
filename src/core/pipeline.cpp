#include "winalign/pipeline.h"
#include "winalign/fastq_parser.h"
#include "winalign/reference_loader.h"
#include "winalign/bam_writer.h"
#include <iostream>
#include <fstream>
#include <cmath>
#include <cuda_runtime.h>

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

// Progress stages and their weights (total = 1.0)
enum class ProgressStage {
    LOADING_REFERENCE = 0,    // 0.00 - 0.10 (10%)
    BUILDING_INDEX,           // 0.10 - 0.20 (10%)
    GPU_INITIALIZATION,       // 0.20 - 0.25 (5%)
    ALIGNMENT,                // 0.25 - 0.90 (65%) - main work
    FINALIZATION              // 0.90 - 1.00 (10%)
};

// Pipeline implementation (PIMPL pattern)
class Pipeline::Impl {
public:
    Impl(const PipelineConfig& cfg) : config_(cfg) {}

    Result<bool> initialize() {
        running_ = true;

        // Stage 1: Load reference (0.00 - 0.10)
        update_progress(0.0, "Loading reference genome");

        reference_loader_ = std::make_unique<ReferenceLoader>(config_.reference_fasta);
        auto load_result = reference_loader_->load();
        if (!load_result.is_ok()) {
            running_ = false;
            return load_result;
        }

        update_progress(0.10, "Reference genome loaded");

        // Stage 2: Build/load FM-index (0.10 - 0.20)
        update_progress(0.10, "Building FM-index");

        if (!reference_loader_->is_indexed()) {
            auto index_result = reference_loader_->build_index();
            if (!index_result.is_ok()) {
                running_ = false;
                return index_result;
            }
        }

        update_progress(0.20, "FM-index ready");

        // Stage 3: Initialize GPU (0.20 - 0.25)
        update_progress(0.20, "Initializing GPU");

        cudaError_t err = cudaSetDevice(config_.gpu_device_id);
        if (err != cudaSuccess) {
            running_ = false;
            return Result<bool>(ErrorCode::CUDA_ERROR,
                              "Failed to set GPU device: " + std::string(cudaGetErrorString(err)));
        }

        // Get GPU properties for reporting
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, config_.gpu_device_id);
        std::cout << "Using GPU: " << prop.name << " (Compute "
                  << prop.major << "." << prop.minor << ")\n";

        // TODO: Allocate GPU memory for reference index
        // TODO: Copy FM-index to GPU

        update_progress(0.25, "GPU initialized");

        return Result<bool>(true);
    }

    Result<bool> run() {
        if (!running_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
        }

        // Stage 4: Main alignment loop (0.25 - 0.90)
        update_progress(0.25, "Starting alignment");

        // Open FASTQ files
        std::unique_ptr<PairedFastqParser> paired_parser;
        std::unique_ptr<FastqParser> single_parser;

        bool is_paired = !config_.read2_fastq.empty();

        if (is_paired) {
            paired_parser = std::make_unique<PairedFastqParser>(
                config_.read1_fastq, config_.read2_fastq);
            auto open_result = paired_parser->open();
            if (!open_result.is_ok()) {
                running_ = false;
                return open_result;
            }
        } else {
            single_parser = std::make_unique<FastqParser>(config_.read1_fastq);
            auto open_result = single_parser->open();
            if (!open_result.is_ok()) {
                running_ = false;
                return open_result;
            }
        }

        // Estimate total reads for progress tracking
        // TODO: Get accurate count or estimate from file size
        uint64_t estimated_total_reads = estimate_total_reads();
        uint64_t processed_reads = 0;

        // Process batches
        while (!cancelled_) {
            // Read batch
            std::vector<ReadPair> read_pairs;
            std::vector<Read> reads;

            size_t batch_count = 0;
            if (is_paired) {
                batch_count = paired_parser->next_batch(read_pairs, config_.batch_size);
            } else {
                batch_count = single_parser->next_batch(reads, config_.batch_size);
            }

            if (batch_count == 0) break; // EOF

            processed_reads += batch_count;

            // Calculate alignment stage progress (0.25 to 0.90)
            double alignment_progress = 0.25 + (0.65 * static_cast<double>(processed_reads) / estimated_total_reads);
            alignment_progress = std::min(alignment_progress, 0.90);

            // Update progress with detailed stage info
            update_progress(alignment_progress,
                          "Aligning reads: " + std::to_string(processed_reads) + "/" +
                          std::to_string(estimated_total_reads));

            // Process batch stages with sub-progress
            // Sub-stage 1: GPU transfer and seeding
            process_seeding(read_pairs, reads, is_paired);

            // Sub-stage 2: GPU alignment
            process_alignment(batch_count);

            // Sub-stage 3: GPU filtering
            process_filtering(batch_count);

            // Sub-stage 4: Transfer results back
            // TODO: Transfer and write results

            // Update metrics
            metrics_.total_reads += batch_count;
        }

        // Close parsers
        if (is_paired) {
            paired_parser->close();
        } else {
            single_parser->close();
        }

        update_progress(0.90, "Alignment complete");

        return Result<bool>(true);
    }

    Result<bool> finalize() {
        // Stage 5: Finalization (0.90 - 1.00)
        update_progress(0.90, "Finalizing output");

        // TODO: Sort BAM if requested
        if (config_.sort_output) {
            update_progress(0.92, "Sorting alignments");
        }

        // TODO: Mark duplicates if requested
        if (config_.mark_duplicates) {
            update_progress(0.95, "Marking duplicates");
        }

        // TODO: Create index if requested
        if (config_.create_index) {
            update_progress(0.97, "Creating BAM index");
        }

        // Write QC metrics
        if (!config_.output_metrics.empty()) {
            write_metrics();
        }

        update_progress(1.0, "Complete");

        running_ = false;
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
    // Progress reporting helper
    void update_progress(double progress, const std::string& stage_info = "") {
        current_progress_ = progress;
        current_stage_info_ = stage_info;

        if (progress_callback_) {
            progress_callback_(progress);
        }
    }

    // Estimate total reads from file size (rough estimate)
    uint64_t estimate_total_reads() {
        // TODO: Better estimation based on first few reads
        // For now, return a placeholder
        return 1000000; // 1M reads default estimate
    }

    // Process seeding stage
    void process_seeding(const std::vector<ReadPair>& pairs,
                        const std::vector<Read>& reads,
                        bool is_paired) {
        // TODO: Transfer reads to GPU
        // TODO: Launch seeding kernels
        // TODO: Extract k-mers and find seeds
    }

    // Process alignment stage
    void process_alignment(size_t batch_count) {
        // TODO: Launch Smith-Waterman kernels
        // TODO: Generate CIGAR strings
        // TODO: Calculate mapping quality

        // Simulate alignment for now
        metrics_.aligned_reads += batch_count * 0.95; // ~95% alignment rate
    }

    // Process filtering stage
    void process_filtering(size_t batch_count) {
        // TODO: Filter by quality
        // TODO: Mark duplicates in batch
        // TODO: Validate pairs
    }

    // Write QC metrics to JSON file
    void write_metrics() {
        std::ofstream out(config_.output_metrics);
        if (!out.is_open()) return;

        out << "{\n";
        out << "  \"total_reads\": " << metrics_.total_reads << ",\n";
        out << "  \"aligned_reads\": " << metrics_.aligned_reads << ",\n";
        out << "  \"alignment_rate\": " << metrics_.alignment_rate() << ",\n";
        out << "  \"properly_paired\": " << metrics_.properly_paired << ",\n";
        out << "  \"duplicates\": " << metrics_.duplicates << ",\n";
        out << "  \"mean_quality\": " << metrics_.mean_quality << ",\n";
        out << "  \"mean_coverage\": " << metrics_.mean_coverage << ",\n";
        out << "  \"mean_insert_size\": " << metrics_.mean_insert_size << "\n";
        out << "}\n";

        out.close();
    }

    PipelineConfig config_;
    QCMetrics metrics_;
    std::function<void(double)> progress_callback_;
    bool running_ = false;
    bool cancelled_ = false;

    // Progress tracking
    double current_progress_ = 0.0;
    std::string current_stage_info_;

    // Pipeline components
    std::unique_ptr<ReferenceLoader> reference_loader_;
    std::unique_ptr<BamWriter> bam_writer_;
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
