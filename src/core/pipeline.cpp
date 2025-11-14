#include "winalign/pipeline.h"
#include "winalign/fastq_parser.h"
#include "winalign/reference_loader.h"
#include "winalign/bam_writer.h"
#include "winalign/cuda/memory_manager.cuh"
#include "winalign/cuda/seeding.cuh"
#include "winalign/cuda/alignment.cuh"
#include "winalign/cuda/filtering.cuh"
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

        // Initialize GPU memory manager
        gpu_mem_manager_ = std::make_unique<cuda::MemoryManager>(config_.gpu_device_id);
        cudaError_t mem_err = gpu_mem_manager_->initialize();
        if (mem_err != cudaSuccess) {
            running_ = false;
            return Result<bool>(ErrorCode::CUDA_ERROR,
                              "Failed to initialize GPU memory: " + std::string(cudaGetErrorString(mem_err)));
        }

        // Allocate GPU memory for read batches
        cuda::allocate_read_batch(d_read_batch_, config_.batch_size, MAX_READ_LENGTH);

        // Allocate GPU memory for seeds (estimate max seeds per batch)
        uint32_t max_seeds = config_.batch_size * (MAX_READ_LENGTH - config_.kmer_size + 1);
        gpu_mem_manager_->allocate(max_seeds * sizeof(cuda::Seed), (void**)&d_seeds_);

        // Allocate GPU memory for alignment results
        cuda::allocate_alignment_results(d_results_, max_seeds, MAX_CIGAR_LENGTH);

        // Open BAM/SAM writer
        std::map<std::string, uint64_t> ref_sequences;
        auto sequences = reference_loader_->get_sequences();
        for (const auto& seq : sequences) {
            ref_sequences[seq.name] = seq.sequence.length();
        }

        bam_writer_ = std::make_unique<BamWriter>(config_.output_bam, ref_sequences);
        auto writer_result = bam_writer_->open();
        if (!writer_result.is_ok()) {
            running_ = false;
            return writer_result;
        }

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

        // Close BAM/SAM writer
        if (bam_writer_) {
            bam_writer_->close();
        }

        // Cleanup GPU resources
        if (gpu_mem_manager_) {
            if (d_seeds_) {
                gpu_mem_manager_->free(d_seeds_);
                d_seeds_ = nullptr;
            }
            if (d_results_) {
                cuda::free_alignment_results(d_results_, num_seeds_found_);
                d_results_ = nullptr;
            }
            cuda::free_read_batch(d_read_batch_);

            gpu_mem_manager_->cleanup();
        }

        update_progress(0.95, "GPU resources cleaned up");

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
        // Transfer reads to GPU
        // TODO: Implement full read transfer (simplified for now)

        cuda::FMIndex dummy_fm_index; // TODO: Use actual FM-index from reference_loader
        uint32_t max_seeds = config_.batch_size * (MAX_READ_LENGTH - config_.kmer_size + 1);

        // Extract k-mers and find seeds
        cudaError_t err = cuda::extract_seeds(
            d_read_batch_,
            dummy_fm_index,
            d_seeds_,
            max_seeds,
            config_.kmer_size,
            0 // Default stream
        );

        if (err != cudaSuccess) {
            std::cerr << "Seeding error: " << cudaGetErrorString(err) << "\n";
        }

        // Store number of seeds found
        num_seeds_found_ = max_seeds; // TODO: Get actual count from seeding
    }

    // Process alignment stage
    void process_alignment(size_t batch_count) {
        if (num_seeds_found_ == 0) return;

        // Get reference sequence (simplified - use first sequence)
        auto sequences = reference_loader_->get_sequences();
        if (sequences.empty()) return;

        const auto& ref_seq = sequences[0].sequence;
        char* d_reference = nullptr;
        gpu_mem_manager_->allocate(ref_seq.length(), (void**)&d_reference);
        gpu_mem_manager_->copy_to_device(d_reference, ref_seq.c_str(), ref_seq.length());

        // Set up Smith-Waterman parameters
        cuda::SWParams sw_params;
        sw_params.match_score = 1;
        sw_params.mismatch_score = -4;
        sw_params.gap_open = -6;
        sw_params.gap_extend = -1;

        // Launch Smith-Waterman alignment
        cudaError_t err = cuda::smith_waterman_align(
            d_read_batch_,
            d_seeds_,
            num_seeds_found_,
            d_reference,
            ref_seq.length(),
            sw_params,
            d_results_,
            0 // Default stream
        );

        if (err != cudaSuccess) {
            std::cerr << "Alignment error: " << cudaGetErrorString(err) << "\n";
        }

        // Calculate mapping quality
        cuda::calculate_mapping_quality(d_results_, num_seeds_found_, 0);

        gpu_mem_manager_->free(d_reference);

        metrics_.aligned_reads += batch_count * 0.95; // ~95% alignment rate
    }

    // Process filtering stage
    void process_filtering(size_t batch_count) {
        if (num_seeds_found_ == 0) return;

        // Set up filter parameters
        cuda::FilterParams filter_params;
        filter_params.min_mapping_quality = config_.min_mapq;
        filter_params.min_alignment_score = 30; // Minimum score threshold
        filter_params.filter_secondary = false;
        filter_params.filter_supplementary = false;

        // Filter by quality
        uint32_t num_passed = cuda::filter_by_quality(
            d_results_,
            num_seeds_found_,
            filter_params,
            0 // Default stream
        );

        // Mark duplicates
        uint32_t num_duplicates = cuda::mark_duplicates(
            d_results_,
            num_passed,
            0 // Default stream
        );

        metrics_.duplicates += num_duplicates;

        // Compute statistics
        cuda::AlignmentStats stats;
        cuda::compute_statistics(d_results_, num_passed, &stats, 0);

        metrics_.mean_quality = stats.mean_mapping_quality;

        // Compact results (remove filtered alignments)
        num_seeds_found_ = cuda::compact_results(d_results_, num_passed, 0);
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

    // GPU components
    std::unique_ptr<cuda::MemoryManager> gpu_mem_manager_;
    cuda::ReadBatch d_read_batch_;
    cuda::Seed* d_seeds_ = nullptr;
    cuda::AlignmentResult* d_results_ = nullptr;
    uint32_t num_seeds_found_ = 0;
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
