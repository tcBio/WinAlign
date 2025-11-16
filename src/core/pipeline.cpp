#include "winalign/pipeline.h"
#include "winalign/fastq_parser.h"
#include "winalign/fastq_worker.h"
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
#include <chrono>
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

constexpr uint32_t MAX_SEEDS_PER_READ = 64;
constexpr uint32_t MAX_HITS_PER_SEED = 8;
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

                            // Create CUDA timing events for profiling
                            cudaEventCreate(&event_h2d_start_);
                            cudaEventCreate(&event_h2d_done_);
                            cudaEventCreate(&event_seeding_start_);
                            cudaEventCreate(&event_seeding_done_);
                            cudaEventCreate(&event_sw_start_);
                            cudaEventCreate(&event_sw_done_);
                            cudaEventCreate(&event_d2h_start_);
                            cudaEventCreate(&event_d2h_done_);
                            Logger::instance().info("Created CUDA timing events for profiling");

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

                            // Initialize multi-stream GPU contexts (Phase 1)
                            if (!initialize_gpu_contexts()) {
                                Logger::instance().warn("Failed to initialize multi-stream GPU contexts. Using single-stream fallback.");
                                // Continue with single-stream mode (old buffers already allocated)
                            } else {
                                Logger::instance().info("Multi-stream GPU scheduler initialized with " +
                                    std::to_string(NUM_GPU_CONTEXTS) + " contexts");
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

            // Allocate pinned host buffers sized for the maximum batch
            bool is_paired = !config_.read2_fastq.empty();
            uint32_t max_reads_per_batch = static_cast<uint32_t>(
                config_.batch_size * (is_paired ? 2u : 1u));
            size_t seq_capacity = static_cast<size_t>(max_reads_per_batch) * MAX_READ_LENGTH;

            if (max_reads_per_batch > 0) {
                if (!pinned_read_sequences_) {
                    if (gpu_mem_manager_->allocate_pinned(
                            seq_capacity,
                            reinterpret_cast<void**>(&pinned_read_sequences_)) == cudaSuccess) {
                        pinned_sequences_capacity_ = seq_capacity;
                    }
                }
                if (!pinned_read_offsets_) {
                    if (gpu_mem_manager_->allocate_pinned(
                            max_reads_per_batch * sizeof(uint32_t),
                            reinterpret_cast<void**>(&pinned_read_offsets_)) == cudaSuccess) {
                        pinned_reads_capacity_ = max_reads_per_batch;
                    }
                }
                if (!pinned_read_lengths_) {
                    gpu_mem_manager_->allocate_pinned(
                        max_reads_per_batch * sizeof(uint32_t),
                        reinterpret_cast<void**>(&pinned_read_lengths_));
                }

                uint32_t max_seeds = max_reads_per_batch *
                    (MAX_READ_LENGTH - static_cast<uint32_t>(config_.kmer_size) + 1);
                if (!pinned_results_) {
                    if (gpu_mem_manager_->allocate_pinned(
                            max_seeds * sizeof(cuda::AlignmentResult),
                            reinterpret_cast<void**>(&pinned_results_)) == cudaSuccess) {
                        pinned_results_capacity_ = max_seeds;
                    }
                }
            }
        } else {
            update_progress(0.25, "Continuing with CPU pipeline");
        }
        Logger::instance().info("Initialization finished");

        return Result<bool>(true);
    }

    // Multi-stream GPU scheduler (Phase 1 + Phase 2)
    Result<bool> run_multi_stream() {
        if (!running_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
        }

        Logger::instance().info("Starting multi-stream GPU scheduler with " +
                               std::to_string(NUM_GPU_CONTEXTS) + " contexts");

        bool is_paired = !config_.read2_fastq.empty();

        // Phase 2: Start multi-threaded FASTQ worker
        int num_io_workers = std::min(4, std::max(1, config_.cpu_threads / 2));
        auto fastq_worker = std::make_unique<FastqWorker>(
            config_.read1_fastq,
            config_.read2_fastq,
            config_.batch_size,
            num_io_workers,
            NUM_GPU_CONTEXTS + 1  // Prefetch one extra batch
        );

        auto start_result = fastq_worker->start();
        if (!start_result.is_ok()) {
            running_ = false;
            return start_result;
        }

        Logger::instance().info("FASTQ worker started with " + std::to_string(num_io_workers) +
                               " IO threads");

        // Estimate total reads for progress tracking
        uint64_t estimated_total_reads = estimate_total_reads();
        uint64_t processed_reads = 0;

        bool parser_eof = false;
        bool all_contexts_done = false;

        // Main scheduler loop
        while (!cancelled_ && !all_contexts_done) {
            all_contexts_done = true;  // Assume done unless we find work

            // === LOAD STAGE: Fill EMPTY contexts with FASTQ data ===
            if (!parser_eof) {
                for (auto& ctx : gpu_contexts_) {
                    if (ctx.state == ContextState::EMPTY) {
                        // Try to get a batch from the worker
                        FastqWorker::ReadBatch batch;
                        if (fastq_worker->get_next_batch(batch)) {
                            ctx.batch_start = std::chrono::high_resolution_clock::now();

                            // Load batch into context
                            load_fastq_into_context_from_worker(ctx, std::move(batch));

                            if (ctx.eof) {
                                parser_eof = true;
                                Logger::instance().info("Reached EOF on FASTQ input");
                            } else if (ctx.read_count > 0) {
                                Logger::instance().info("Loaded batch into context " +
                                    std::to_string(ctx.context_id) + ": " +
                                    std::to_string(ctx.read_count) + " reads");

                                // Prepare for GPU
                                if (!prepare_context_for_gpu(ctx)) {
                                    Logger::instance().warn("Failed to prepare context " +
                                        std::to_string(ctx.context_id) + " for GPU");
                                    ctx.state = ContextState::EMPTY;
                                } else {
                                    ctx.state = ContextState::READY;
                                    all_contexts_done = false;
                                }
                            }
                        } else {
                            // No more batches available
                            if (fastq_worker->is_eof()) {
                                parser_eof = true;
                            }
                        }
                    }
                }
            }

            // === H2D TRANSFER STAGE: Launch async H2D for READY contexts ===
            for (auto& ctx : gpu_contexts_) {
                if (ctx.state == ContextState::READY) {
                    cudaError_t err = launch_h2d_transfer(ctx);
                    if (err != cudaSuccess) {
                        Logger::instance().warn("H2D transfer failed for context " +
                            std::to_string(ctx.context_id) + ": " + cudaGetErrorString(err));
                        ctx.state = ContextState::EMPTY;
                    } else {
                        ctx.state = ContextState::TRANSFERRING;
                        all_contexts_done = false;
                    }
                }
            }

            // === SEEDING STAGE: Launch seeding for contexts that finished H2D ===
            for (auto& ctx : gpu_contexts_) {
                if (ctx.state == ContextState::TRANSFERRING) {
                    cudaError_t err = cudaEventQuery(ctx.event_h2d_done);
                    if (err == cudaSuccess) {
                        // H2D complete, collect timing
                        float h2d_time = 0.0f;
                        cudaEventElapsedTime(&h2d_time,
                            ctx.event_h2d_done, ctx.event_h2d_done); // Placeholder
                        ctx.timing.t_h2d = h2d_time;

                        // Launch seeding
                        err = launch_seeding(ctx);
                        if (err != cudaSuccess) {
                            Logger::instance().warn("GPU seeding failed for context " +
                                std::to_string(ctx.context_id) + ": " + cudaGetErrorString(err));
                            // Fall back to CPU or skip
                            ctx.state = ContextState::ALIGNING;  // Skip to alignment
                        } else {
                            ctx.state = ContextState::SEEDING;
                        }
                        all_contexts_done = false;
                    } else if (err != cudaErrorNotReady) {
                        Logger::instance().error("Event query error for context " +
                            std::to_string(ctx.context_id));
                        ctx.state = ContextState::EMPTY;
                    } else {
                        all_contexts_done = false;  // Still waiting
                    }
                }
            }

            // === ALIGNMENT STAGE: Launch SW for contexts that finished seeding ===
            for (auto& ctx : gpu_contexts_) {
                if (ctx.state == ContextState::SEEDING) {
                    cudaError_t err = cudaEventQuery(ctx.event_seeding_done);
                    if (err == cudaSuccess) {
                        // Seeding complete, collect timing
                        float seed_time = 0.0f;
                        cudaEventElapsedTime(&seed_time,
                            ctx.event_h2d_done, ctx.event_seeding_done);
                        ctx.timing.t_gpu_seed = seed_time;

                        Logger::instance().info("Context " + std::to_string(ctx.context_id) +
                            " seeding complete: " + std::to_string(ctx.num_seeds) + " seeds");

                        // Launch alignment
                        err = launch_alignment(ctx);
                        if (err != cudaSuccess) {
                            Logger::instance().warn("GPU alignment failed for context " +
                                std::to_string(ctx.context_id));
                            ctx.state = ContextState::COPYING_BACK;  // Skip to D2H
                        } else {
                            ctx.state = ContextState::ALIGNING;
                        }
                        all_contexts_done = false;
                    } else if (err != cudaErrorNotReady) {
                        Logger::instance().error("Event query error for context " +
                            std::to_string(ctx.context_id));
                        ctx.state = ContextState::EMPTY;
                    } else {
                        all_contexts_done = false;  // Still waiting
                    }
                }
            }

            // === D2H TRANSFER STAGE: Launch async D2H for contexts that finished alignment ===
            for (auto& ctx : gpu_contexts_) {
                if (ctx.state == ContextState::ALIGNING) {
                    cudaError_t err = cudaEventQuery(ctx.event_sw_done);
                    if (err == cudaSuccess) {
                        // Alignment complete, collect timing
                        float align_time = 0.0f;
                        cudaEventElapsedTime(&align_time,
                            ctx.event_seeding_done, ctx.event_sw_done);
                        ctx.timing.t_gpu_align = align_time;

                        Logger::instance().info("Context " + std::to_string(ctx.context_id) +
                            " alignment complete");

                        // Launch D2H transfer
                        err = launch_d2h_transfer(ctx);
                        if (err != cudaSuccess) {
                            Logger::instance().warn("D2H transfer failed for context " +
                                std::to_string(ctx.context_id));
                            ctx.state = ContextState::EMPTY;
                        } else {
                            ctx.state = ContextState::COPYING_BACK;
                        }
                        all_contexts_done = false;
                    } else if (err != cudaErrorNotReady) {
                        Logger::instance().error("Event query error for context " +
                            std::to_string(ctx.context_id));
                        ctx.state = ContextState::EMPTY;
                    } else {
                        all_contexts_done = false;  // Still waiting
                    }
                }
            }

            // === HOST PROCESSING STAGE: Process contexts that finished D2H ===
            for (auto& ctx : gpu_contexts_) {
                if (ctx.state == ContextState::COPYING_BACK) {
                    cudaError_t err = cudaEventQuery(ctx.event_d2h_done);
                    if (err == cudaSuccess) {
                        // D2H complete, collect timing
                        float d2h_time = 0.0f;
                        cudaEventElapsedTime(&d2h_time,
                            ctx.event_sw_done, ctx.event_d2h_done);
                        ctx.timing.t_d2h = d2h_time;

                        ctx.state = ContextState::DONE;
                        all_contexts_done = false;
                    } else if (err != cudaErrorNotReady) {
                        Logger::instance().error("Event query error for context " +
                            std::to_string(ctx.context_id));
                        ctx.state = ContextState::EMPTY;
                    } else {
                        all_contexts_done = false;  // Still waiting
                    }
                }

                if (ctx.state == ContextState::DONE) {
                    // Process results (aggregate, write BAM)
                    Logger::instance().info("Processing results for context " +
                        std::to_string(ctx.context_id));

                    process_context_results(ctx);

                    processed_reads += ctx.read_count;

                    // Update progress
                    double alignment_progress = 0.25 + (0.65 * static_cast<double>(processed_reads) / estimated_total_reads);
                    alignment_progress = std::min(alignment_progress, 0.90);
                    update_progress(alignment_progress,
                                   "Aligning reads: " + std::to_string(processed_reads) + "/" +
                                   std::to_string(estimated_total_reads));

                    // Mark context as EMPTY for reuse
                    ctx.state = ContextState::EMPTY;
                    all_contexts_done = false;
                }
            }

            // Check if we're truly done
            if (parser_eof) {
                bool any_active = false;
                for (const auto& ctx : gpu_contexts_) {
                    if (ctx.state != ContextState::EMPTY) {
                        any_active = true;
                        break;
                    }
                }
                all_contexts_done = !any_active;
            } else {
                all_contexts_done = false;
            }

            // Small sleep to avoid busy-waiting (optional)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        // Final synchronization - wait for all contexts to finish
        Logger::instance().info("Waiting for all GPU contexts to complete...");
        for (auto& ctx : gpu_contexts_) {
            if (ctx.stream) {
                cudaStreamSynchronize(ctx.stream);
            }
        }

        // Stop FASTQ worker
        fastq_worker->stop();

        update_progress(0.90, "Multi-stream alignment complete");
        Logger::instance().info("Multi-stream alignment loop finished: " +
                               std::to_string(processed_reads) + " reads processed");

        return Result<bool>(true);
    }

    Result<bool> run() {
        if (!running_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR, "Pipeline not initialized");
        }

        // Use multi-stream scheduler if available
        if (!gpu_contexts_.empty()) {
            return run_multi_stream();
        }

        // Fall back to single-stream mode
        Logger::instance().info("Using single-stream pipeline (multi-stream not available)");


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

        // Host-side batch buffers for simple double buffering
        struct HostBatch {
            std::vector<ReadPair> pairs;
            std::vector<Read> singles;
            size_t count = 0;
            bool eof = false;
        };

        auto read_next_batch = [&](HostBatch& batch) {
            batch.pairs.clear();
            batch.singles.clear();
            batch.count = 0;
            batch.eof = false;

            if (is_paired) {
                batch.count = paired_parser->next_batch(batch.pairs, config_.batch_size);
            } else {
                batch.count = single_parser->next_batch(batch.singles, config_.batch_size);
            }

            if (batch.count == 0) {
                batch.eof = true;
            }
        };

        HostBatch current_batch;
        HostBatch next_batch;

        // Prime first batch
        read_next_batch(current_batch);
        if (current_batch.eof || current_batch.count == 0) {
            Logger::instance().info("No reads found in input FASTQ files.");
        } else {
            // Launch prefetch for second batch
            std::thread prefetch_thread;
            bool have_prefetch = false;

            if (!cancelled_) {
                prefetch_thread = std::thread(read_next_batch, std::ref(next_batch));
                have_prefetch = true;
            }

            // Process batches
            while (!cancelled_ && !current_batch.eof && current_batch.count > 0) {
                const size_t batch_count = current_batch.count;
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
                    auto gpu_result = process_gpu_batch(
                        current_batch.pairs,
                        current_batch.singles,
                        is_paired);
                    if (gpu_result.is_ok()) {
                        gpu_processed = true;
                    } else {
                        Logger::instance().warn(
                            "GPU batch failed: " + gpu_result.message +
                            ". Falling back to CPU alignment.");
                    }
                }

                if (!gpu_processed) {
                    process_cpu_alignment(
                        current_batch.pairs,
                        current_batch.singles,
                        is_paired);
                }

                // Wait for prefetch to complete and rotate batches
                if (have_prefetch) {
                    prefetch_thread.join();
                    have_prefetch = false;
                }
                current_batch = std::move(next_batch);

                if (current_batch.eof || current_batch.count == 0) {
                    Logger::instance().info("Parser reached EOF after processing " +
                                            std::to_string(processed_reads) + " entries.");
                    break;
                }

                // Start prefetch for next iteration
                if (!cancelled_) {
                    prefetch_thread = std::thread(read_next_batch, std::ref(next_batch));
                    have_prefetch = true;
                }
            }

            if (have_prefetch) {
                prefetch_thread.join();
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

        // Log performance summary
        log_performance_summary();

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

            // Destroy CUDA timing events
            if (event_h2d_start_) cudaEventDestroy(event_h2d_start_);
            if (event_h2d_done_) cudaEventDestroy(event_h2d_done_);
            if (event_seeding_start_) cudaEventDestroy(event_seeding_start_);
            if (event_seeding_done_) cudaEventDestroy(event_seeding_done_);
            if (event_sw_start_) cudaEventDestroy(event_sw_start_);
            if (event_sw_done_) cudaEventDestroy(event_sw_done_);
            if (event_d2h_start_) cudaEventDestroy(event_d2h_start_);
            if (event_d2h_done_) cudaEventDestroy(event_d2h_done_);

            // Cleanup multi-stream GPU contexts (Phase 1)
            cleanup_gpu_contexts();

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

    // Pinned host buffers for faster transfers
    char* pinned_read_sequences_ = nullptr;
    uint32_t* pinned_read_offsets_ = nullptr;
    uint32_t* pinned_read_lengths_ = nullptr;
    cuda::AlignmentResult* pinned_results_ = nullptr;
    size_t pinned_sequences_capacity_ = 0;
    size_t pinned_reads_capacity_ = 0;
    size_t pinned_results_capacity_ = 0;
    std::vector<Alignment> gpu_alignment_buffer_;
    std::vector<uint32_t> reads_without_seeds_;

    uint32_t num_seeds_found_ = 0;
    double mapping_quality_sum_ = 0.0;

    // === Phase 0: Profiling and Performance Counters ===

    // Per-batch timing (in milliseconds)
    struct BatchTiming {
        double t_read = 0.0;        // FASTQ read time
        double t_h2d = 0.0;         // Host-to-device transfer
        double t_gpu_seed = 0.0;    // GPU seeding kernel time
        double t_gpu_align = 0.0;   // GPU alignment kernel time
        double t_d2h = 0.0;         // Device-to-host transfer
        double t_write = 0.0;       // BAM write time
        double t_total = 0.0;       // Total batch time
    };

    // Cumulative timing statistics
    BatchTiming cumulative_timing_;
    uint32_t batch_count_ = 0;

    // Performance counters
    struct PerformanceCounters {
        uint64_t gpu_aligned_reads = 0;
        uint64_t unmapped_reads = 0;
        uint64_t reads_without_seeds = 0;
        uint64_t total_gpu_seeds = 0;
        uint64_t batches_processed = 0;

        double avg_seeds_per_read() const {
            return gpu_aligned_reads > 0
                ? static_cast<double>(total_gpu_seeds) / gpu_aligned_reads
                : 0.0;
        }
    };
    PerformanceCounters perf_counters_;

    // CUDA timing events
    cudaEvent_t event_h2d_start_ = nullptr;
    cudaEvent_t event_h2d_done_ = nullptr;
    cudaEvent_t event_seeding_start_ = nullptr;
    cudaEvent_t event_seeding_done_ = nullptr;
    cudaEvent_t event_sw_start_ = nullptr;
    cudaEvent_t event_sw_done_ = nullptr;
    cudaEvent_t event_d2h_start_ = nullptr;
    cudaEvent_t event_d2h_done_ = nullptr;

    // Helper: get elapsed time between two CUDA events (in milliseconds)
    float get_cuda_event_time(cudaEvent_t start, cudaEvent_t stop) {
        float elapsed_ms = 0.0f;
        cudaEventElapsedTime(&elapsed_ms, start, stop);
        return elapsed_ms;
    }

    // Helper: log timing and performance stats
    void log_batch_timing(const BatchTiming& timing, uint32_t batch_num) {
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

    void log_performance_summary() {
        if (batch_count_ == 0) return;

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

    // === End Phase 0 ===

    // === Phase 1: Multi-stream GPU Batch Context ===

    enum class ContextState {
        EMPTY,          // Ready to be filled
        LOADING,        // Reading FASTQ data
        READY,          // H2D transfer ready
        TRANSFERRING,   // H2D transfer in progress
        SEEDING,        // GPU seeding in progress
        ALIGNING,       // GPU alignment in progress
        COPYING_BACK,   // D2H transfer in progress
        DONE,           // Ready for host processing
        PROCESSING      // Host-side processing (BAM write)
    };

    struct GpuBatchContext {
        // State
        ContextState state = ContextState::EMPTY;
        uint32_t context_id = 0;

        // Host data
        std::vector<ReadPair> host_pairs;
        std::vector<Read> host_singles;
        size_t read_count = 0;
        bool is_paired = false;
        bool eof = false;

        // Flattened host buffers (for GPU transfer)
        std::vector<char> host_read_sequences;
        std::vector<uint32_t> host_read_offsets;
        std::vector<uint32_t> host_read_lengths;
        std::vector<ReadView> read_views;

        // Pinned host buffers (for async transfer)
        char* pinned_sequences = nullptr;
        uint32_t* pinned_offsets = nullptr;
        uint32_t* pinned_lengths = nullptr;
        cuda::Seed* pinned_seeds = nullptr;
        cuda::AlignmentResult* pinned_results = nullptr;

        // Device buffers
        cuda::ReadBatch d_read_batch;
        cuda::Seed* d_seeds = nullptr;
        cuda::AlignmentResult* d_results = nullptr;

        // Seed and result data
        std::vector<cuda::Seed> host_seeds;
        std::vector<cuda::AlignmentResult> host_results;
        uint32_t num_seeds = 0;

        // CUDA stream and events
        cudaStream_t stream = nullptr;
        cudaEvent_t event_h2d_done = nullptr;
        cudaEvent_t event_seeding_done = nullptr;
        cudaEvent_t event_sw_done = nullptr;
        cudaEvent_t event_d2h_done = nullptr;

        // Timing for this batch
        BatchTiming timing;
        std::chrono::high_resolution_clock::time_point batch_start;
    };

    // Multi-stream context management
    static constexpr int NUM_GPU_CONTEXTS = 3;  // Overlap 3 batches
    std::vector<GpuBatchContext> gpu_contexts_;

    // Initialize GPU contexts
    bool initialize_gpu_contexts() {
        if (!gpu_enabled_ || !gpu_mem_manager_) {
            return false;
        }

        gpu_contexts_.resize(NUM_GPU_CONTEXTS);

        bool is_paired = !config_.read2_fastq.empty();
        uint32_t max_reads_per_batch = static_cast<uint32_t>(
            config_.batch_size * (is_paired ? 2u : 1u));
        uint32_t max_seeds = max_reads_per_batch *
            (MAX_READ_LENGTH - static_cast<uint32_t>(config_.kmer_size) + 1);

        for (int i = 0; i < NUM_GPU_CONTEXTS; ++i) {
            auto& ctx = gpu_contexts_[i];
            ctx.context_id = i;
            ctx.state = ContextState::EMPTY;

            // Create dedicated stream for this context
            cudaError_t err = cudaStreamCreate(&ctx.stream);
            if (err != cudaSuccess) {
                Logger::instance().error("Failed to create stream for context " +
                    std::to_string(i) + ": " + cudaGetErrorString(err));
                return false;
            }

            // Create events for this context
            cudaEventCreate(&ctx.event_h2d_done);
            cudaEventCreate(&ctx.event_seeding_done);
            cudaEventCreate(&ctx.event_sw_done);
            cudaEventCreate(&ctx.event_d2h_done);

            // Allocate device buffers for this context
            err = cuda::allocate_read_batch(ctx.d_read_batch, max_reads_per_batch, MAX_READ_LENGTH);
            if (err != cudaSuccess) {
                Logger::instance().error("Failed to allocate read batch for context " +
                    std::to_string(i));
                return false;
            }

            err = gpu_mem_manager_->allocate(
                max_seeds * sizeof(cuda::Seed),
                (void**)&ctx.d_seeds);
            if (err != cudaSuccess) {
                Logger::instance().error("Failed to allocate seeds for context " +
                    std::to_string(i));
                return false;
            }

            err = cuda::allocate_alignment_results(
                ctx.d_results, max_seeds, MAX_CIGAR_LENGTH);
            if (err != cudaSuccess) {
                Logger::instance().error("Failed to allocate results for context " +
                    std::to_string(i));
                return false;
            }

            // Allocate pinned host buffers for async transfer
            size_t seq_capacity = static_cast<size_t>(max_reads_per_batch) * MAX_READ_LENGTH;

            gpu_mem_manager_->allocate_pinned(seq_capacity, (void**)&ctx.pinned_sequences);
            gpu_mem_manager_->allocate_pinned(
                max_reads_per_batch * sizeof(uint32_t), (void**)&ctx.pinned_offsets);
            gpu_mem_manager_->allocate_pinned(
                max_reads_per_batch * sizeof(uint32_t), (void**)&ctx.pinned_lengths);
            gpu_mem_manager_->allocate_pinned(
                max_seeds * sizeof(cuda::Seed), (void**)&ctx.pinned_seeds);
            gpu_mem_manager_->allocate_pinned(
                max_seeds * sizeof(cuda::AlignmentResult), (void**)&ctx.pinned_results);

            Logger::instance().info("Initialized GPU context " + std::to_string(i));
        }

        return true;
    }

    // Cleanup GPU contexts
    void cleanup_gpu_contexts() {
        for (auto& ctx : gpu_contexts_) {
            if (ctx.stream) {
                cudaStreamSynchronize(ctx.stream);
                cudaStreamDestroy(ctx.stream);
            }
            if (ctx.event_h2d_done) cudaEventDestroy(ctx.event_h2d_done);
            if (ctx.event_seeding_done) cudaEventDestroy(ctx.event_seeding_done);
            if (ctx.event_sw_done) cudaEventDestroy(ctx.event_sw_done);
            if (ctx.event_d2h_done) cudaEventDestroy(ctx.event_d2h_done);

            if (ctx.d_seeds && gpu_mem_manager_) {
                gpu_mem_manager_->free(ctx.d_seeds);
            }
            if (ctx.d_results) {
                cuda::free_alignment_results(ctx.d_results, ctx.num_seeds);
            }
            cuda::free_read_batch(ctx.d_read_batch);

            if (ctx.pinned_sequences) gpu_mem_manager_->free_pinned(ctx.pinned_sequences);
            if (ctx.pinned_offsets) gpu_mem_manager_->free_pinned(ctx.pinned_offsets);
            if (ctx.pinned_lengths) gpu_mem_manager_->free_pinned(ctx.pinned_lengths);
            if (ctx.pinned_seeds) gpu_mem_manager_->free_pinned(ctx.pinned_seeds);
            if (ctx.pinned_results) gpu_mem_manager_->free_pinned(ctx.pinned_results);
        }
        gpu_contexts_.clear();
    }

    // === End Phase 1 Context Definition ===

    // === Phase 1: Multi-stream Scheduler Helper Functions ===

    // Load FASTQ batch into context from worker
    void load_fastq_into_context_from_worker(GpuBatchContext& ctx, FastqWorker::ReadBatch&& batch) {
        using namespace std::chrono;
        auto read_start = high_resolution_clock::now();

        ctx.host_pairs = std::move(batch.pairs);
        ctx.host_singles = std::move(batch.singles);
        ctx.read_count = batch.count;
        ctx.is_paired = batch.is_paired;
        ctx.eof = batch.eof;

        auto read_end = high_resolution_clock::now();
        ctx.timing.t_read = duration_cast<microseconds>(read_end - read_start).count() / 1000.0;
    }

    // Load FASTQ batch into context (legacy for single-stream)
    void load_fastq_into_context(GpuBatchContext& ctx,
                                  PairedFastqParser* paired_parser,
                                  FastqParser* single_parser,
                                  bool is_paired) {
        using namespace std::chrono;
        auto read_start = high_resolution_clock::now();

        ctx.host_pairs.clear();
        ctx.host_singles.clear();
        ctx.read_count = 0;
        ctx.is_paired = is_paired;

        if (is_paired && paired_parser) {
            ctx.read_count = paired_parser->next_batch(ctx.host_pairs, config_.batch_size);
        } else if (!is_paired && single_parser) {
            ctx.read_count = single_parser->next_batch(ctx.host_singles, config_.batch_size);
        }

        ctx.eof = (ctx.read_count == 0);

        auto read_end = high_resolution_clock::now();
        ctx.timing.t_read = duration_cast<microseconds>(read_end - read_start).count() / 1000.0;
    }

    // Prepare context for GPU by flattening reads into pinned buffers
    bool prepare_context_for_gpu(GpuBatchContext& ctx) {
        ctx.host_read_sequences.clear();
        ctx.host_read_offsets.clear();
        ctx.host_read_lengths.clear();
        ctx.read_views.clear();

        auto append_read = [&](const Read& read, bool is_second) {
            ReadView view;
            view.read = &read;
            view.is_paired = ctx.is_paired;
            view.is_second = is_second;
            ctx.read_views.push_back(view);

            uint32_t offset = static_cast<uint32_t>(ctx.host_read_sequences.size());
            ctx.host_read_offsets.push_back(offset);

            uint32_t clamped_len = static_cast<uint32_t>(
                std::min<size_t>(read.sequence.size(), MAX_READ_LENGTH));
            ctx.host_read_lengths.push_back(clamped_len);

            if (!read.sequence.empty()) {
                ctx.host_read_sequences.insert(
                    ctx.host_read_sequences.end(),
                    read.sequence.begin(),
                    read.sequence.begin() + clamped_len);
            }
        };

        if (ctx.is_paired) {
            for (const auto& pair : ctx.host_pairs) {
                append_read(pair.read1, false);
                append_read(pair.read2, true);
            }
        } else {
            for (const auto& read : ctx.host_singles) {
                append_read(read, false);
            }
        }

        // Copy to pinned buffers
        if (!ctx.read_views.empty() && ctx.pinned_sequences && ctx.pinned_offsets && ctx.pinned_lengths) {
            if (!ctx.host_read_sequences.empty()) {
                std::memcpy(ctx.pinned_sequences,
                           ctx.host_read_sequences.data(),
                           ctx.host_read_sequences.size());
            }
            std::memcpy(ctx.pinned_offsets,
                       ctx.host_read_offsets.data(),
                       ctx.host_read_offsets.size() * sizeof(uint32_t));
            std::memcpy(ctx.pinned_lengths,
                       ctx.host_read_lengths.data(),
                       ctx.host_read_lengths.size() * sizeof(uint32_t));
        }

        ctx.d_read_batch.num_reads = static_cast<uint32_t>(ctx.read_views.size());
        return !ctx.read_views.empty();
    }

    // Launch async H2D transfer
    cudaError_t launch_h2d_transfer(GpuBatchContext& ctx) {
        if (ctx.read_views.empty()) {
            return cudaSuccess;
        }

        const size_t num_reads = ctx.read_views.size();
        size_t seq_bytes = ctx.host_read_sequences.size();

        // Async copy sequences
        if (seq_bytes > 0) {
            cudaError_t err = cudaMemcpyAsync(
                ctx.d_read_batch.sequences,
                ctx.pinned_sequences,
                seq_bytes,
                cudaMemcpyHostToDevice,
                ctx.stream);
            if (err != cudaSuccess) return err;
        }

        // Async copy offsets and lengths
        if (num_reads > 0) {
            size_t meta_bytes = num_reads * sizeof(uint32_t);

            cudaError_t err = cudaMemcpyAsync(
                ctx.d_read_batch.offsets,
                ctx.pinned_offsets,
                meta_bytes,
                cudaMemcpyHostToDevice,
                ctx.stream);
            if (err != cudaSuccess) return err;

            err = cudaMemcpyAsync(
                ctx.d_read_batch.lengths,
                ctx.pinned_lengths,
                meta_bytes,
                cudaMemcpyHostToDevice,
                ctx.stream);
            if (err != cudaSuccess) return err;
        }

        // Record event when H2D is done
        cudaEventRecord(ctx.event_h2d_done, ctx.stream);

        return cudaSuccess;
    }

    // Launch GPU seeding
    cudaError_t launch_seeding(GpuBatchContext& ctx) {
        uint32_t step = std::max<uint32_t>(1, config_.kmer_size / 3);

        cudaError_t err = cuda::generate_gpu_seeds(
            ctx.d_read_batch,
            d_fm_index_,
            ctx.d_seeds,
            MAX_SEEDS_PER_READ,
            config_.kmer_size,
            step,
            ctx.num_seeds,
            ctx.stream);

        if (err == cudaSuccess) {
            cudaEventRecord(ctx.event_seeding_done, ctx.stream);
        }

        return err;
    }

    // Launch GPU alignment
    cudaError_t launch_alignment(GpuBatchContext& ctx) {
        if (ctx.num_seeds == 0) {
            return cudaSuccess;
        }

        cuda::SWParams sw_params;
        sw_params.match_score = config_.scores.match;
        sw_params.mismatch_score = config_.scores.mismatch;
        sw_params.gap_open = config_.scores.gap_open;
        sw_params.gap_extend = config_.scores.gap_extend;

        cudaError_t err = cuda::smith_waterman_align(
            ctx.d_read_batch,
            ctx.d_seeds,
            ctx.num_seeds,
            d_reference_,
            reference_length_,
            sw_params,
            ctx.d_results,
            ctx.stream);
        if (err != cudaSuccess) return err;

        err = cuda::calculate_mapping_quality(
            ctx.d_results, ctx.num_seeds, ctx.stream);
        if (err != cudaSuccess) return err;

        cudaEventRecord(ctx.event_sw_done, ctx.stream);

        return cudaSuccess;
    }

    // Launch async D2H transfer
    cudaError_t launch_d2h_transfer(GpuBatchContext& ctx) {
        if (ctx.num_seeds == 0) {
            cudaEventRecord(ctx.event_d2h_done, ctx.stream);
            return cudaSuccess;
        }

        size_t bytes = ctx.num_seeds * sizeof(cuda::AlignmentResult);

        cudaError_t err = cudaMemcpyAsync(
            ctx.pinned_results,
            ctx.d_results,
            bytes,
            cudaMemcpyDeviceToHost,
            ctx.stream);

        if (err == cudaSuccess) {
            cudaEventRecord(ctx.event_d2h_done, ctx.stream);
        }

        return err;
    }

    // Process context results (host-side aggregation and BAM write)
    void process_context_results(GpuBatchContext& ctx) {
        using namespace std::chrono;

        // Copy results from pinned to host buffers
        ctx.host_results.resize(ctx.num_seeds);
        if (ctx.num_seeds > 0 && ctx.pinned_results) {
            std::memcpy(ctx.host_results.data(),
                       ctx.pinned_results,
                       ctx.num_seeds * sizeof(cuda::AlignmentResult));
        }

        // Find best alignment per read
        struct BestResult {
            bool has_value = false;
            cuda::AlignmentResult result;
        };

        std::vector<BestResult> best_results(ctx.read_views.size());
        for (const auto& device_result : ctx.host_results) {
            if (device_result.read_id >= best_results.size()) {
                continue;
            }
            auto& slot = best_results[device_result.read_id];
            if (!slot.has_value || device_result.score > slot.result.score) {
                slot.has_value = true;
                slot.result = device_result;
            }
        }

        // Build alignments and write to BAM
        std::vector<Alignment> alignments;
        std::vector<Read> output_reads;
        alignments.reserve(ctx.read_views.size());
        output_reads.reserve(ctx.read_views.size());

        auto write_start = high_resolution_clock::now();

        for (size_t idx = 0; idx < ctx.read_views.size(); ++idx) {
            const auto& view = ctx.read_views[idx];
            Alignment alignment;
            bool mapped = false;

            if (best_results[idx].has_value && best_results[idx].result.score > 0) {
                alignment = build_alignment_from_gpu_result(
                    best_results[idx].result, view);
                mapped = (alignment.flag & sam_flags::UNMAPPED) == 0;
            } else {
                if (config_.fast_mode) {
                    alignment = build_unmapped_alignment(*view.read,
                                                         view.is_paired,
                                                         view.is_second);
                } else {
                    alignment = align_read_cpu(*view.read,
                                               view.is_paired,
                                               view.is_second,
                                               mapped);
                }
            }

            alignments.push_back(alignment);
            output_reads.push_back(*view.read);
            update_metrics(mapped, alignment.mapping_quality);

            // Update performance counters
            if (mapped) {
                perf_counters_.gpu_aligned_reads++;
            } else {
                perf_counters_.unmapped_reads++;
            }
        }

        if (!alignments.empty() && bam_writer_) {
            bam_writer_->write_batch(alignments, output_reads);
        }

        auto write_end = high_resolution_clock::now();
        ctx.timing.t_write = duration_cast<microseconds>(write_end - write_start).count() / 1000.0;

        // Update performance counters
        perf_counters_.total_gpu_seeds += ctx.num_seeds;
        perf_counters_.batches_processed++;

        // Calculate total batch time
        auto batch_end = high_resolution_clock::now();
        ctx.timing.t_total = duration_cast<microseconds>(batch_end - ctx.batch_start).count() / 1000.0;

        // Update cumulative timing
        cumulative_timing_.t_read += ctx.timing.t_read;
        cumulative_timing_.t_h2d += ctx.timing.t_h2d;
        cumulative_timing_.t_gpu_seed += ctx.timing.t_gpu_seed;
        cumulative_timing_.t_gpu_align += ctx.timing.t_gpu_align;
        cumulative_timing_.t_d2h += ctx.timing.t_d2h;
        cumulative_timing_.t_write += ctx.timing.t_write;
        cumulative_timing_.t_total += ctx.timing.t_total;
        batch_count_++;

        // Log timing for this batch
        log_batch_timing(ctx.timing, batch_count_);
    }

    // === End Phase 1 Helper Functions ===

    Result<bool> process_gpu_batch(const std::vector<ReadPair>& pairs,
                                   const std::vector<Read>& singles,
                                   bool is_paired) {
        using namespace std::chrono;
        auto batch_start = high_resolution_clock::now();
        BatchTiming timing;

        if (!gpu_enabled_ || !gpu_mem_manager_ || !fm_index_view_ || !d_reference_) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                                "GPU pipeline not initialized");
        }

        if (!prepare_gpu_batch(pairs, singles, is_paired)) {
            return Result<bool>(ErrorCode::RUNTIME_ERROR,
                                "Failed to prepare GPU read batch");
        }

        // === H2D Transfer with timing ===
        if (event_h2d_start_) cudaEventRecord(event_h2d_start_, compute_stream_);

        if (!gpu_read_views_.empty()) {
            const size_t num_reads = gpu_read_views_.size();
            if (!host_read_sequences_.empty()) {
                size_t seq_bytes = host_read_sequences_.size();
                if (pinned_read_sequences_ && seq_bytes <= pinned_sequences_capacity_) {
                    std::memcpy(pinned_read_sequences_,
                                host_read_sequences_.data(),
                                seq_bytes);
                    auto err_copy = gpu_mem_manager_->copy_to_device(
                        d_read_batch_.sequences,
                        pinned_read_sequences_,
                        seq_bytes,
                        compute_stream_);
                    if (err_copy != cudaSuccess) {
                        return Result<bool>(ErrorCode::CUDA_ERROR,
                                            "Failed to copy reads to device");
                    }
                } else {
                    auto err_copy = gpu_mem_manager_->copy_to_device(
                        d_read_batch_.sequences,
                        host_read_sequences_.data(),
                        seq_bytes,
                        compute_stream_);
                    if (err_copy != cudaSuccess) {
                        return Result<bool>(ErrorCode::CUDA_ERROR,
                                            "Failed to copy reads to device");
                    }
                }
            }

            if (num_reads > 0) {
                size_t bytes = num_reads * sizeof(uint32_t);
                const uint32_t* lengths_src = host_read_lengths_.data();
                const uint32_t* offsets_src = host_read_offsets_.data();

                if (pinned_read_lengths_ && num_reads <= pinned_reads_capacity_) {
                    std::memcpy(pinned_read_lengths_,
                                host_read_lengths_.data(),
                                bytes);
                    lengths_src = pinned_read_lengths_;
                }
                if (pinned_read_offsets_ && num_reads <= pinned_reads_capacity_) {
                    std::memcpy(pinned_read_offsets_,
                                host_read_offsets_.data(),
                                bytes);
                    offsets_src = pinned_read_offsets_;
                }

                auto err_copy = gpu_mem_manager_->copy_to_device(
                    d_read_batch_.lengths,
                    lengths_src,
                    bytes,
                    compute_stream_);
                if (err_copy != cudaSuccess) {
                    auto message = std::string("Failed to copy read lengths: ") +
                                   cudaGetErrorString(err_copy);
                    return Result<bool>(ErrorCode::CUDA_ERROR, message);
                }

                err_copy = gpu_mem_manager_->copy_to_device(
                    d_read_batch_.offsets,
                    offsets_src,
                    bytes,
                    compute_stream_);
                if (err_copy != cudaSuccess) {
                    auto message = std::string("Failed to copy read offsets: ") +
                                   cudaGetErrorString(err_copy);
                    return Result<bool>(ErrorCode::CUDA_ERROR, message);
                }
            }
            d_read_batch_.num_reads = static_cast<uint32_t>(num_reads);
        }

        if (event_h2d_done_) cudaEventRecord(event_h2d_done_, compute_stream_);

        // === GPU Seeding with timing ===
        if (event_seeding_start_) cudaEventRecord(event_seeding_start_, compute_stream_);

        bool used_cpu_seeding = false;
        uint32_t total_seeds = 0;
        uint32_t step = std::max<uint32_t>(1, config_.kmer_size / 3);
        Logger::instance().info("GPU seeding batch of " +
            std::to_string(d_read_batch_.num_reads) + " reads");
        cudaError_t seed_err = cuda::generate_gpu_seeds(
            d_read_batch_,
            d_fm_index_,
            d_seeds_,
            MAX_SEEDS_PER_READ,
            config_.kmer_size,
            step,
            total_seeds,
            compute_stream_);

        if (event_seeding_done_) cudaEventRecord(event_seeding_done_, compute_stream_);

        if (seed_err != cudaSuccess || total_seeds == 0) {
            std::string err_str = (seed_err == cudaSuccess)
                ? "no seeds generated"
                : std::string(cudaGetErrorString(seed_err));
            Logger::instance().warn("GPU seeding failed or produced no seeds (" +
                err_str + ").");
            if (config_.fast_mode) {
                Logger::instance().info("Fast mode enabled: skipping CPU FM-index seeding fallback for this batch");
                num_seeds_found_ = 0;
            } else {
                Logger::instance().warn("Falling back to CPU FM-index seeding");
                if (!build_seeds_from_fm_index()) {
                    return Result<bool>(ErrorCode::RUNTIME_ERROR,
                                        "Unable to generate seeds from FM-index");
                }
                used_cpu_seeding = true;
            }
        } else {
            Logger::instance().info("GPU FM-index seeding complete: " +
                std::to_string(total_seeds) + " seeds");
            num_seeds_found_ = total_seeds;
        }

        if (used_cpu_seeding) {
            num_seeds_found_ = static_cast<uint32_t>(host_seeds_.size());
            if (num_seeds_found_ == 0) {
                return Result<bool>(ErrorCode::RUNTIME_ERROR,
                                    "CPU FM-index seeding produced no seeds");
            }
            auto err_copy = gpu_mem_manager_->copy_to_device(
                d_seeds_,
                host_seeds_.data(),
                num_seeds_found_ * sizeof(cuda::Seed));
            if (err_copy != cudaSuccess) {
                auto message = std::string("Failed to copy fallback seeds to device: ") +
                               cudaGetErrorString(err_copy);
                return Result<bool>(ErrorCode::CUDA_ERROR, message);
            }
        }

        // === Smith-Waterman Alignment with timing ===
        if (event_sw_start_) cudaEventRecord(event_sw_start_, compute_stream_);

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

        if (event_sw_done_) cudaEventRecord(event_sw_done_, compute_stream_);

        // === D2H Transfer with timing ===
        if (event_d2h_start_) cudaEventRecord(event_d2h_start_, compute_stream_);

        host_results_.resize(num_seeds_found_);
        if (num_seeds_found_ > 0) {
            size_t bytes = num_seeds_found_ * sizeof(cuda::AlignmentResult);
            if (pinned_results_ && num_seeds_found_ <= pinned_results_capacity_) {
                err = gpu_mem_manager_->copy_to_host(
                    pinned_results_,
                    d_results_,
                    bytes,
                    compute_stream_);
                if (err != cudaSuccess) {
                    return Result<bool>(ErrorCode::CUDA_ERROR,
                                        "Failed to copy alignment results to pinned host buffer");
                }
                // Ensure results are visible on host
                cudaError_t sync_err = cudaStreamSynchronize(compute_stream_);
                if (sync_err != cudaSuccess) {
                    return Result<bool>(ErrorCode::CUDA_ERROR,
                                        "Failed to synchronize after copying alignment results");
                }
                std::memcpy(host_results_.data(), pinned_results_, bytes);
            } else {
                err = gpu_mem_manager_->copy_to_host(
                    host_results_.data(),
                    d_results_,
                    bytes,
                    compute_stream_);
                if (err != cudaSuccess) {
                    return Result<bool>(ErrorCode::CUDA_ERROR,
                                        "Failed to copy alignment results to host");
                }
                cudaStreamSynchronize(compute_stream_);
            }
        }

        if (event_d2h_done_) cudaEventRecord(event_d2h_done_, compute_stream_);

        // Synchronize to collect timing
        cudaStreamSynchronize(compute_stream_);
        if (event_h2d_start_ && event_h2d_done_) {
            timing.t_h2d = get_cuda_event_time(event_h2d_start_, event_h2d_done_);
        }
        if (event_seeding_start_ && event_seeding_done_) {
            timing.t_gpu_seed = get_cuda_event_time(event_seeding_start_, event_seeding_done_);
        }
        if (event_sw_start_ && event_sw_done_) {
            timing.t_gpu_align = get_cuda_event_time(event_sw_start_, event_sw_done_);
        }
        if (event_d2h_start_ && event_d2h_done_) {
            timing.t_d2h = get_cuda_event_time(event_d2h_start_, event_d2h_done_);
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
                if (config_.fast_mode) {
                    alignment = build_unmapped_alignment(*view.read,
                                                         view.is_paired,
                                                         view.is_second);
                    mapped = false;
                } else {
                    alignment = align_read_cpu(*view.read,
                                               view.is_paired,
                                               view.is_second,
                                               mapped);
                }
            }

            alignments.push_back(alignment);
            output_reads.push_back(*view.read);
            update_metrics(mapped, alignment.mapping_quality);

            // Update performance counters
            if (mapped) {
                perf_counters_.gpu_aligned_reads++;
            } else {
                perf_counters_.unmapped_reads++;
            }
        }

        // === BAM Write with timing ===
        auto write_start = high_resolution_clock::now();
        if (!alignments.empty() && bam_writer_) {
            bam_writer_->write_batch(alignments, output_reads);
        }
        auto write_end = high_resolution_clock::now();
        timing.t_write = duration_cast<microseconds>(write_end - write_start).count() / 1000.0;

        // Calculate total batch time
        auto batch_end = high_resolution_clock::now();
        timing.t_total = duration_cast<microseconds>(batch_end - batch_start).count() / 1000.0;

        // Update cumulative timing
        cumulative_timing_.t_h2d += timing.t_h2d;
        cumulative_timing_.t_gpu_seed += timing.t_gpu_seed;
        cumulative_timing_.t_gpu_align += timing.t_gpu_align;
        cumulative_timing_.t_d2h += timing.t_d2h;
        cumulative_timing_.t_write += timing.t_write;
        cumulative_timing_.t_total += timing.t_total;
        batch_count_++;

        // Update performance counters
        perf_counters_.total_gpu_seeds += num_seeds_found_;
        perf_counters_.reads_without_seeds += reads_without_seeds_.size();
        perf_counters_.batches_processed++;

        // Log timing for this batch
        log_batch_timing(timing, batch_count_);

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
