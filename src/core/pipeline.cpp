#include "winalign/pipeline.h"
#include "winalign/fastq_parser.h"
#include "winalign/reference_loader.h"
#include "winalign/bam_writer.h"
#include "winalign/cuda/alignment.cuh"
#include "winalign/cuda/filtering.cuh"
#include "winalign/cuda/memory_manager.cuh"
#include "winalign/cuda/seeding.cuh"
#include "winalign/fm_index.h"
#include "winalign/logger.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <atomic>
#include <sstream>
#include <thread>
#include <cuda_runtime.h>

namespace winalign {

namespace {
bool cuda_driver_available() {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("nvcuda.dll");
    if (!lib) {
        return false;
    }
    FreeLibrary(lib);
    return true;
#else
    void* handle = dlopen("libcuda.so", RTLD_LAZY);
    if (!handle) {
        return false;
    }
    dlclose(handle);
    return true;
#endif
}

constexpr uint32_t MAX_SEEDS_PER_READ = 32;
constexpr uint32_t MAX_HITS_PER_SEED = 4;
constexpr uint32_t WINDOW_FLANK = 64;
} // anonymous namespace

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

    struct ReadView {
        const Read* read = nullptr;
        bool is_paired = false;
        bool is_second = false;
    };

    Result<bool> initialize() {
        running_ = true;
        Logger::instance().info("Initializing pipeline");

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
        fm_index_view_ = reference_loader_->get_fm_index();
        concatenated_reference_ = &reference_loader_->concatenated_sequence();
        chromosome_offsets_ = reference_loader_->chromosome_offsets();

        // Stage 3: Initialize GPU (0.20 - 0.25)
        if (config_.use_gpu) {
            update_progress(0.20, "Initializing GPU");

            if (!cuda_driver_available()) {
                Logger::instance().warn("CUDA driver not detected. Falling back to CPU pipeline.");
            } else {
                Logger::instance().info("CUDA driver detected, querying devices");
                int device_count = 0;
                cudaError_t device_err = cudaGetDeviceCount(&device_count);
                if (device_err != cudaSuccess) {
                    Logger::instance().warn(std::string("cudaGetDeviceCount failed: ") +
                                            cudaGetErrorString(device_err) +
                                            ". Falling back to CPU pipeline.");
                } else if (device_count == 0) {
                    Logger::instance().warn("No CUDA-capable device detected. Falling back to CPU pipeline.");
                } else if (config_.gpu_device_id >= device_count) {
                    Logger::instance().warn("Requested GPU ID " + std::to_string(config_.gpu_device_id) +
                                            " exceeds detected devices (" + std::to_string(device_count) +
                                            "). Falling back to CPU pipeline.");
                } else {
                    Logger::instance().info("Detected " + std::to_string(device_count) + " CUDA device(s)");
                    Logger::instance().info("Setting active GPU to device " + std::to_string(config_.gpu_device_id));
                    cudaError_t err = cudaSetDevice(config_.gpu_device_id);
                    if (err != cudaSuccess) {
                        Logger::instance().warn(std::string("Failed to set GPU device: ") +
                                                cudaGetErrorString(err) +
                                                ". Falling back to CPU pipeline.");
                    } else {
                        cudaDeviceProp prop;
                        err = cudaGetDeviceProperties(&prop, config_.gpu_device_id);
                        if (err != cudaSuccess) {
                            Logger::instance().warn(std::string("Failed to query GPU properties: ") +
                                                    cudaGetErrorString(err) +
                                                    ". Falling back to CPU pipeline.");
                        } else {
                            std::cout << "Using GPU: " << prop.name << " (Compute "
                                      << prop.major << "." << prop.minor << ")\n";
                            Logger::instance().info(std::string("Using GPU: ") + prop.name);

                            gpu_mem_manager_ = std::make_unique<cuda::MemoryManager>(config_.gpu_device_id);
                            cudaError_t mem_err = gpu_mem_manager_->initialize();
                            if (mem_err != cudaSuccess) {
                                running_ = false;
                                auto message = std::string("Failed to initialize GPU memory: ") +
                                               cudaGetErrorString(mem_err);
                                Logger::instance().error(message);
                                return Result<bool>(ErrorCode::CUDA_ERROR, message);
                            }

                        uint32_t max_reads_per_batch = config_.batch_size *
                            (config_.read2_fastq.empty() ? 1u : 2u);

                        Logger::instance().info("Allocating read batch buffers ("
                                + std::to_string(max_reads_per_batch) + " reads)");
                            cudaError_t alloc_err = cuda::allocate_read_batch(
                                d_read_batch_, max_reads_per_batch, MAX_READ_LENGTH);
                            if (alloc_err != cudaSuccess) {
                                auto message = std::string("Failed to allocate read batch: ") +
                                               cudaGetErrorString(alloc_err);
                                Logger::instance().error(message);
                                return Result<bool>(ErrorCode::CUDA_ERROR, message);
                            }

                        uint32_t max_seeds =
                            max_reads_per_batch * (MAX_READ_LENGTH - config_.kmer_size + 1);
                            Logger::instance().info("Allocating seed buffer for up to "
                                + std::to_string(max_seeds) + " seeds");
                            auto seed_err = gpu_mem_manager_->allocate(
                                max_seeds * sizeof(cuda::Seed),
                                (void**)&d_seeds_);
                            if (seed_err != cudaSuccess) {
                                auto message = std::string("Failed to allocate seed buffer: ") +
                                               cudaGetErrorString(seed_err);
                                Logger::instance().error(message);
                                return Result<bool>(ErrorCode::CUDA_ERROR, message);
                            }

                            Logger::instance().info("Allocating alignment results buffer");
                            auto res_err = cuda::allocate_alignment_results(
                                d_results_, max_seeds, MAX_CIGAR_LENGTH);
                            if (res_err != cudaSuccess) {
                                auto message = std::string("Failed to allocate alignment results: ") +
                                               cudaGetErrorString(res_err);
                                Logger::instance().error(message);
                                return Result<bool>(ErrorCode::CUDA_ERROR, message);
                            }

                            if (!compute_stream_) {
                                cudaStreamCreate(&compute_stream_);
                            }

                            if (fm_index_view_) {
                                const FMIndexView view = fm_index_view_->get_view();
                                cuda::HostFMIndexView host_view;
                                host_view.bwt = view.bwt;
                                host_view.length = view.length;
                                host_view.c_table = view.c_table;
                                host_view.occ_table = view.occ_table;
                                host_view.occ_entries = view.occ_entries;
                                host_view.suffix_array = view.suffix_array;
                                host_view.suffix_length = view.suffix_length;
                                host_view.occ_interval = view.occ_interval;

                                Logger::instance().info("Allocating device FM-index buffers (" +
                                    std::to_string(host_view.length) + " bp, " +
                                    std::to_string(host_view.occ_entries) + " occ entries, " +
                                    std::to_string(host_view.suffix_length) + " SA entries)");
                                err = cuda::allocate_fm_index(
                                    d_fm_index_,
                                    host_view.length,
                                    host_view.occ_entries,
                                    host_view.suffix_length);
                                if (err != cudaSuccess) {
                                    auto message = std::string("Failed to allocate device FM-index: ") +
                                                   cudaGetErrorString(err);
                                    Logger::instance().error(message);
                                    return Result<bool>(ErrorCode::CUDA_ERROR, message);
                                }

                                Logger::instance().info("Copying FM-index to device");
                                err = cuda::copy_fm_index_to_device(
                                    d_fm_index_,
                                    host_view,
                                    compute_stream_);
                                if (err != cudaSuccess) {
                                    auto message = std::string("Failed to copy FM-index to device: ") +
                                                   cudaGetErrorString(err);
                                    Logger::instance().error(message);
                                    return Result<bool>(ErrorCode::CUDA_ERROR, message);
                                }
                                d_fm_index_ready_ = true;
                            }

                            if (concatenated_reference_) {
                                reference_length_ = concatenated_reference_->size();
                                if (reference_length_ > 0) {
                                    Logger::instance().info("Copying reference (" +
                                        std::to_string(reference_length_) + " bp) to device");
                                    auto ref_err = gpu_mem_manager_->allocate(
                                        reference_length_,
                                        reinterpret_cast<void**>(&d_reference_));
                                    if (ref_err != cudaSuccess) {
                                        auto message = std::string("Failed to allocate device reference: ") +
                                                       cudaGetErrorString(ref_err);
                                        Logger::instance().error(message);
                                        return Result<bool>(ErrorCode::CUDA_ERROR, message);
                                    }
                                    ref_err = gpu_mem_manager_->copy_to_device(
                                        d_reference_,
                                        concatenated_reference_->data(),
                                        reference_length_);
                                    if (ref_err != cudaSuccess) {
                                        auto message = std::string("Failed to copy reference to device: ") +
                                                       cudaGetErrorString(ref_err);
                                        Logger::instance().error(message);
                                        return Result<bool>(ErrorCode::CUDA_ERROR, message);
                                    }
                                }
                            }

                            gpu_enabled_ = true;
                        }
                    }
                }
            }
        } else {
            Logger::instance().info("GPU disabled (--cpu-only). Using CPU pipeline.");
        }

        // Open BAM/SAM writer
        std::map<std::string, uint64_t> ref_sequences;
        const auto& sequences = reference_loader_->get_sequences();
        for (const auto& kv : sequences) {
            ref_sequences[kv.first] = kv.second.sequence.length();
        }

        bam_writer_ = std::make_unique<BamWriter>(config_.output_bam, ref_sequences);
        auto writer_result = bam_writer_->open();
        if (!writer_result.is_ok()) {
            running_ = false;
            return writer_result;
        }

        if (gpu_enabled_) {
            update_progress(0.25, "GPU initialized");
        } else {
            update_progress(0.25, "Continuing with CPU pipeline");
        }
        Logger::instance().info("Initialization finished");

        return Result<bool>(true);
    }

    Result<bool> run() {
        if (!running_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
        }

        // Stage 4: Main alignment loop (0.25 - 0.90)
        update_progress(0.25, "Starting alignment");
        Logger::instance().info("Alignment loop started");

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

            if (batch_count == 0) {
                std::cout << "No reads available in this batch. Ending alignment loop.\n";
                Logger::instance().info("Parser reached EOF after processing " +
                                        std::to_string(processed_reads) + " entries.");
                break; // EOF
            }
            std::cout << "Read batch of " << batch_count
                      << (is_paired ? " pairs\n" : " reads\n");
            Logger::instance().info("Read batch size: " + std::to_string(batch_count) +
                                    (is_paired ? " pairs" : " reads"));

            processed_reads += batch_count;

            // Calculate alignment stage progress (0.25 to 0.90)
            double alignment_progress = 0.25 + (0.65 * static_cast<double>(processed_reads) / estimated_total_reads);
            alignment_progress = std::min(alignment_progress, 0.90);

            // Update progress with detailed stage info
            update_progress(alignment_progress,
                          "Aligning reads: " + std::to_string(processed_reads) + "/" +
                          std::to_string(estimated_total_reads));

            bool gpu_processed = false;
            if (gpu_enabled_) {
                auto gpu_result = process_gpu_batch(read_pairs, reads, is_paired);
                if (gpu_result.is_ok()) {
                    gpu_processed = true;
                } else {
                    Logger::instance().warn(
                        "GPU batch failed: " + gpu_result.message +
                        ". Falling back to CPU alignment.");
                }
            }

            if (!gpu_processed) {
                process_cpu_alignment(read_pairs, reads, is_paired);
            }
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
            if (d_fm_index_ready_) {
                cuda::free_fm_index(d_fm_index_);
                d_fm_index_ready_ = false;
            }
            if (d_reference_) {
                gpu_mem_manager_->free(d_reference_);
                d_reference_ = nullptr;
            }
            if (compute_stream_) {
                cudaStreamDestroy(compute_stream_);
                compute_stream_ = nullptr;
            }

            gpu_mem_manager_->cleanup();
        }

        update_progress(0.95, "GPU resources cleaned up");

        // Write QC metrics
        if (!config_.output_metrics.empty()) {
            write_metrics();
        }

        update_progress(1.0, "Complete");
        Logger::instance().info("Pipeline finalized");

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

        if (!stage_info.empty() && stage_info != last_logged_stage_) {
            Logger::instance().info(stage_info);
            last_logged_stage_ = stage_info;
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
        filter_params.min_mapping_quality = config_.min_mapping_quality;
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
    std::string last_logged_stage_;

    // Pipeline components
    std::unique_ptr<ReferenceLoader> reference_loader_;
    std::unique_ptr<BamWriter> bam_writer_;

    // GPU components
    std::unique_ptr<cuda::MemoryManager> gpu_mem_manager_;
    cuda::ReadBatch d_read_batch_;
    cuda::Seed* d_seeds_ = nullptr;
    cuda::AlignmentResult* d_results_ = nullptr;
    cuda::FMIndex d_fm_index_;
    bool d_fm_index_ready_ = false;
    char* d_reference_ = nullptr;
    uint64_t reference_length_ = 0;
    cudaStream_t compute_stream_ = nullptr;
    bool gpu_enabled_ = false;
    const FMIndex* fm_index_view_ = nullptr;
    const std::string* concatenated_reference_ = nullptr;
    std::vector<ChromosomeOffset> chromosome_offsets_;

    std::vector<ReadView> gpu_read_views_;
    std::vector<char> host_read_sequences_;
    std::vector<uint32_t> host_read_offsets_;
    std::vector<uint32_t> host_read_lengths_;
    std::vector<cuda::Seed> host_seeds_;
    std::vector<cuda::AlignmentResult> host_results_;
    std::vector<Alignment> gpu_alignment_buffer_;
    std::vector<uint32_t> reads_without_seeds_;

    uint32_t num_seeds_found_ = 0;
    double mapping_quality_sum_ = 0.0;

    Result<bool> process_gpu_batch(const std::vector<ReadPair>& pairs,
                                   const std::vector<Read>& singles,
                                   bool is_paired) {
        if (!gpu_enabled_ || !gpu_mem_manager_ || !fm_index_view_ || !d_reference_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                                "GPU pipeline not initialized");
        }

        if (!prepare_gpu_batch(pairs, singles, is_paired)) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                                "Failed to prepare GPU read batch");
        }

        if (!build_seeds_from_fm_index()) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                                "Unable to generate seeds from FM-index");
        }

        if (!gpu_read_views_.empty()) {
            const size_t num_reads = gpu_read_views_.size();
            if (!host_read_sequences_.empty()) {
                auto err_copy = gpu_mem_manager_->copy_to_device(
                    d_read_batch_.sequences,
                    host_read_sequences_.data(),
                    host_read_sequences_.size());
                if (err_copy != cudaSuccess) {
                    return Result<bool>(ErrorCode::CUDA_ERROR,
                                        "Failed to copy reads to device");
                }
            }

            auto err_copy = gpu_mem_manager_->copy_to_device(
                d_read_batch_.lengths,
                host_read_lengths_.data(),
                num_reads * sizeof(uint32_t));
            if (err_copy != cudaSuccess) {
                auto message = std::string("Failed to copy read lengths: ") +
                               cudaGetErrorString(err_copy);
                return Result<bool>(ErrorCode::CUDA_ERROR, message);
            }

            err_copy = gpu_mem_manager_->copy_to_device(
                d_read_batch_.offsets,
                host_read_offsets_.data(),
                num_reads * sizeof(uint32_t));
            if (err_copy != cudaSuccess) {
                auto message = std::string("Failed to copy read offsets: ") +
                               cudaGetErrorString(err_copy);
                return Result<bool>(ErrorCode::CUDA_ERROR, message);
            }
            d_read_batch_.num_reads = static_cast<uint32_t>(num_reads);
        }

        if (d_read_batch_.num_reads > 0) {
            auto err_copy = gpu_mem_manager_->copy_to_device(
                d_seeds_,
                host_seeds_.data(),
                num_seeds_found_ * sizeof(cuda::Seed));
            if (err_copy != cudaSuccess) {
                auto message = std::string("Failed to copy seeds to device: ") +
                               cudaGetErrorString(err_copy);
                return Result<bool>(ErrorCode::CUDA_ERROR, message);
            }
        }

        cuda::SWParams sw_params;
        sw_params.match_score = config_.scores.match;
        sw_params.mismatch_score = config_.scores.mismatch;
        sw_params.gap_open = config_.scores.gap_open;
        sw_params.gap_extend = config_.scores.gap_extend;

        cudaError_t err = cuda::smith_waterman_align(
            d_read_batch_,
            d_seeds_,
            num_seeds_found_,
            d_reference_,
            reference_length_,
            sw_params,
            d_results_,
            compute_stream_);
        if (err != cudaSuccess) {
            return Result<bool>(ErrorCode::CUDA_ERROR,
                                "smith_waterman_align failed");
        }

        err = cuda::calculate_mapping_quality(
            d_results_, num_seeds_found_, compute_stream_);
        if (err != cudaSuccess) {
            return Result<bool>(ErrorCode::CUDA_ERROR,
                                "calculate_mapping_quality failed");
        }

        host_results_.resize(num_seeds_found_);
        err = gpu_mem_manager_->copy_to_host(
            host_results_.data(),
            d_results_,
            num_seeds_found_ * sizeof(cuda::AlignmentResult));
        if (err != cudaSuccess) {
            return Result<bool>(ErrorCode::CUDA_ERROR,
                                "Failed to copy alignment results to host");
        }

        struct BestResult {
            bool has_value = false;
            cuda::AlignmentResult result;
        };

        std::vector<BestResult> best_results(gpu_read_views_.size());
        for (const auto& device_result : host_results_) {
            if (device_result.read_id >= best_results.size()) {
                continue;
            }
            auto& slot = best_results[device_result.read_id];
            if (!slot.has_value || device_result.score > slot.result.score) {
                slot.has_value = true;
                slot.result = device_result;
            }
        }

        std::vector<Alignment> alignments;
        std::vector<Read> output_reads;
        alignments.reserve(gpu_read_views_.size());
        output_reads.reserve(gpu_read_views_.size());

        for (size_t idx = 0; idx < gpu_read_views_.size(); ++idx) {
            const auto& view = gpu_read_views_[idx];
            Alignment alignment;
            bool mapped = false;

            if (best_results[idx].has_value &&
                best_results[idx].result.score > 0) {
                alignment = build_alignment_from_gpu_result(
                    best_results[idx].result, view);
                mapped = (alignment.flag & sam_flags::UNMAPPED) == 0;
            } else {
                alignment = align_read_cpu(*view.read,
                                           view.is_paired,
                                           view.is_second,
                                           mapped);
            }

            alignments.push_back(alignment);
            output_reads.push_back(*view.read);
            update_metrics(mapped, alignment.mapping_quality);
        }

        if (!alignments.empty() && bam_writer_) {
            bam_writer_->write_batch(alignments, output_reads);
        }

        return Result<bool>(true);
    }

    bool prepare_gpu_batch(const std::vector<ReadPair>& pairs,
                           const std::vector<Read>& singles,
                           bool is_paired) {
        gpu_read_views_.clear();
        host_read_sequences_.clear();
        host_read_offsets_.clear();
        host_read_lengths_.clear();
        d_read_batch_.num_reads = 0;

        auto append_read = [&](const Read& read, bool second) {
            ReadView view;
            view.read = &read;
            view.is_paired = is_paired;
            view.is_second = second;
            gpu_read_views_.push_back(view);

            uint32_t offset = static_cast<uint32_t>(host_read_sequences_.size());
            host_read_offsets_.push_back(offset);

            uint32_t clamped_len = static_cast<uint32_t>(
                std::min<size_t>(read.sequence.size(), MAX_READ_LENGTH));
            host_read_lengths_.push_back(clamped_len);

            if (!read.sequence.empty()) {
                host_read_sequences_.insert(
                    host_read_sequences_.end(),
                    read.sequence.begin(),
                    read.sequence.begin() + clamped_len);
            }
        };

        if (is_paired) {
            for (const auto& pair : pairs) {
                append_read(pair.read1, false);
                append_read(pair.read2, true);
            }
        } else {
            for (const auto& read : singles) {
                append_read(read, false);
            }
        }

        d_read_batch_.num_reads = static_cast<uint32_t>(gpu_read_views_.size());
        return !gpu_read_views_.empty();
    }

    bool build_seeds_from_fm_index() {
        host_seeds_.clear();
        reads_without_seeds_.clear();

        if (!fm_index_view_) {
            return false;
        }

        const size_t k = config_.kmer_size;
        if (k == 0) {
            return false;
        }

        const size_t read_count = gpu_read_views_.size();
        if (read_count == 0) {
            return false;
        }

        size_t hardware_threads = std::thread::hardware_concurrency();
        if (hardware_threads == 0) hardware_threads = 4;
        const size_t num_threads = std::min(read_count, hardware_threads);
        const size_t chunk = (read_count + num_threads - 1) / num_threads;
        const size_t log_interval = 5000;
        std::atomic<size_t> processed_reads{0};

        std::vector<std::vector<cuda::Seed>> thread_seeds(num_threads);
        std::vector<std::vector<uint32_t>> thread_missing(num_threads);

        auto worker = [&](size_t tid) {
            size_t begin = tid * chunk;
            size_t end = std::min(read_count, begin + chunk);
            if (begin >= end) return;

            auto& local_seeds = thread_seeds[tid];
            auto& local_missing = thread_missing[tid];
            local_seeds.reserve((end - begin) * MAX_SEEDS_PER_READ);

            for (size_t read_idx = begin; read_idx < end; ++read_idx) {
                const auto& view = gpu_read_views_[read_idx];
                const std::string& seq = view.read->sequence;

                if (seq.size() < k) {
                    local_missing.push_back(static_cast<uint32_t>(read_idx));
                    continue;
                }

                size_t seeds_for_read = 0;
                size_t step = std::max<size_t>(1, k / 2);

                for (size_t offset = 0;
                     offset + k <= seq.size() && seeds_for_read < MAX_SEEDS_PER_READ;
                     offset += step) {
                    std::vector<Position> matches;
                    fm_index_view_->search(
                        seq.data() + offset,
                        k,
                        matches,
                        MAX_HITS_PER_SEED);

                    for (const auto pos : matches) {
                        cuda::Seed seed{};
                        seed.position = pos;
                        seed.read_id = static_cast<uint32_t>(read_idx);
                        seed.read_offset = static_cast<uint32_t>(offset);
                        seed.length = static_cast<uint16_t>(k);
                        seed.mismatches = 0;
                        local_seeds.push_back(seed);
                        seeds_for_read++;
                        if (seeds_for_read >= MAX_SEEDS_PER_READ) {
                            break;
                        }
                    }
                }

                if (seeds_for_read == 0) {
                    local_missing.push_back(static_cast<uint32_t>(read_idx));
                }

                size_t prev_total = processed_reads.fetch_add(1);
                size_t new_total = prev_total + 1;
                if (log_interval > 0 &&
                    new_total / log_interval > prev_total / log_interval) {
                    Logger::instance().info("Seeded " +
                        std::to_string(new_total) + " reads");
                }
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(num_threads);
        for (size_t tid = 0; tid < num_threads; ++tid) {
            threads.emplace_back(worker, tid);
        }
        for (auto& t : threads) {
            t.join();
        }

        size_t total_seeds = 0;
        for (const auto& local : thread_seeds) {
            total_seeds += local.size();
        }
        host_seeds_.reserve(total_seeds);
        for (auto& local : thread_seeds) {
            host_seeds_.insert(host_seeds_.end(),
                               local.begin(),
                               local.end());
        }

        for (auto& local_missing : thread_missing) {
            reads_without_seeds_.insert(reads_without_seeds_.end(),
                                        local_missing.begin(),
                                        local_missing.end());
        }

        num_seeds_found_ = static_cast<uint32_t>(host_seeds_.size());
        Logger::instance().info("FM-index seeding complete: " +
            std::to_string(num_seeds_found_) + " seeds");
        return num_seeds_found_ > 0;
    }

    Alignment build_alignment_from_gpu_result(const cuda::AlignmentResult& result,
                                              const ReadView& view) {
        Alignment alignment = build_unmapped_alignment(*view.read,
                                                       view.is_paired,
                                                       view.is_second);

        std::string contig;
        uint64_t local_offset = 0;
        if (!reference_loader_->map_global_position(result.position,
                                                    contig,
                                                    local_offset)) {
            return alignment;
        }

        alignment.reference_name = contig;
        alignment.position = local_offset;
        alignment.mapping_quality = result.mapping_quality;
        alignment.alignment_score = result.score;
        alignment.flag &= ~sam_flags::UNMAPPED;
        alignment.cigar = std::to_string(view.read->sequence.size()) + "M";
        return alignment;
    }

    const ChromosomeOffset* find_contig(uint64_t global_pos) const {
        for (const auto& chrom : chromosome_offsets_) {
            if (global_pos >= chrom.start &&
                global_pos < chrom.start + chrom.length) {
                return &chrom;
            }
        }
        return nullptr;
    }

    void process_cpu_alignment(const std::vector<ReadPair>& pairs,
                               const std::vector<Read>& singles,
                               bool is_paired) {
        std::vector<Alignment> alignments;
        std::vector<Read> output_reads;

        if (is_paired) {
            for (const auto& pair : pairs) {
                bool mapped1 = false;
                bool mapped2 = false;
                Alignment aln1 = align_read_cpu(pair.read1, true, false, mapped1);
                Alignment aln2 = align_read_cpu(pair.read2, true, true, mapped2);

                alignments.push_back(aln1);
                output_reads.push_back(pair.read1);
                update_metrics(mapped1, aln1.mapping_quality);

                alignments.push_back(aln2);
                output_reads.push_back(pair.read2);
                update_metrics(mapped2, aln2.mapping_quality);

                if (mapped1 && mapped2 &&
                    aln1.reference_name == aln2.reference_name) {
                    metrics_.properly_paired++;
                }
            }
        } else {
            for (const auto& read : singles) {
                bool mapped = false;
                Alignment aln = align_read_cpu(read, false, false, mapped);
                alignments.push_back(aln);
                output_reads.push_back(read);
                update_metrics(mapped, aln.mapping_quality);
            }
        }

        if (!alignments.empty() && bam_writer_) {
            bam_writer_->write_batch(alignments, output_reads);
        }
    }

    Alignment build_unmapped_alignment(const Read& read,
                                       bool is_paired,
                                       bool is_second) {
        Alignment alignment;
        alignment.read_id = read.id;
        alignment.reference_name = "*";
        alignment.position = 0;
        alignment.mapping_quality = 0;
        alignment.flag = 0;
        alignment.cigar = "*";
        alignment.alignment_score = 0;
        alignment.is_primary = true;

        if (is_paired) {
            alignment.flag |= sam_flags::PAIRED;
            alignment.flag |= is_second ? sam_flags::READ2 : sam_flags::READ1;
        }

        alignment.flag |= sam_flags::UNMAPPED;
        return alignment;
    }

    Alignment align_read_cpu(const Read& read,
                             bool is_paired,
                             bool is_second,
                             bool& mapped) {
        mapped = false;

        Alignment alignment = build_unmapped_alignment(read, is_paired, is_second);

        if (read.sequence.empty()) {
            return alignment;
        }

        for (const auto& kv : reference_loader_->get_sequences()) {
            const auto& ref_name = kv.first;
            const auto& ref_seq = kv.second.sequence;
            auto pos = ref_seq.find(read.sequence);
            if (pos != std::string::npos) {
                alignment.reference_name = ref_name;
                alignment.position = static_cast<uint64_t>(pos);
                alignment.mapping_quality = 60;
                alignment.cigar = std::to_string(read.sequence.size()) + "M";
                mapped = true;
                alignment.flag &= ~sam_flags::UNMAPPED;
                break;
            }
        }

        if (!mapped) {
            alignment.flag |= sam_flags::UNMAPPED;
        }

        return alignment;
    }

    void update_metrics(bool mapped, uint8_t mapq) {
        metrics_.total_reads++;
        if (mapped) {
            metrics_.aligned_reads++;
            mapping_quality_sum_ += mapq;
        }
        metrics_.mean_quality = metrics_.aligned_reads > 0
            ? mapping_quality_sum_ / static_cast<double>(metrics_.aligned_reads)
            : 0.0;
    }
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
