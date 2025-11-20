/**
 * @file pipeline_initialization.cpp
 * @brief Pipeline initialization and resource allocation implementation
 *
 * Handles the complex initialization sequence for the alignment pipeline
 * including reference loading, GPU setup, and output file preparation.
 */

#include "pipeline_initialization.h"
#include "pipeline_gpu_context.h"
#include "pipeline_internal.h"
#include "winalign/pipeline.h"
#include "winalign/reference_loader.h"
#include "winalign/bam_writer.h"
#include "winalign/logger.h"
#include "winalign/cuda/memory_manager.cuh"
#include "winalign/cuda/fm_index.cuh"
#include <cuda_runtime.h>
#include <string>
#include <iostream>

namespace winalign {

// Helper function to check CUDA driver availability
extern "C" {
    cudaError_t __cudaRegisterFatBinary(void*) __attribute__((weak));
}

static bool cuda_driver_available() {
    return __cudaRegisterFatBinary != nullptr;
}

namespace internal {

PipelineInitializer::PipelineInitializer(
    const PipelineConfig& config,
    std::function<void(double, const std::string&)> update_progress_fn)
    : config_(config)
    , update_progress_fn_(update_progress_fn)
    , fm_index_view_(nullptr)
    , concatenated_reference_(nullptr)
    , gpu_enabled_(false)
    , d_seeds_(nullptr)
    , d_results_(nullptr)
    , d_reference_(nullptr)
    , reference_length_(0)
    , compute_stream_(nullptr)
    , pinned_read_sequences_(nullptr)
    , pinned_read_offsets_(nullptr)
    , pinned_read_lengths_(nullptr)
    , pinned_results_(nullptr)
    , pinned_sequences_capacity_(0)
    , pinned_reads_capacity_(0)
    , pinned_results_capacity_(0)
{
    d_read_batch_ = {};
    d_fm_index_ = {};
}

Result<bool> PipelineInitializer::initialize() {
    Logger::instance().info("Initializing pipeline");

    // Stage 1: Load reference and FM-index (0.00 - 0.20)
    auto ref_result = initialize_reference();
    if (!ref_result.is_ok()) {
        return ref_result;
    }

    // Stage 2: Initialize GPU if enabled (0.20 - 0.25)
    if (config_.use_gpu) {
        auto gpu_result = initialize_gpu();
        if (!gpu_result.is_ok()) {
            return gpu_result;
        }
    } else {
        Logger::instance().info("GPU disabled (--cpu-only). Using CPU pipeline.");
        if (update_progress_fn_) {
            update_progress_fn_(0.25, "Continuing with CPU pipeline");
        }
    }

    // Stage 3: Initialize BAM writer
    auto bam_result = initialize_bam_writer();
    if (!bam_result.is_ok()) {
        return bam_result;
    }

    // Stage 4: Allocate pinned buffers if GPU enabled
    if (gpu_enabled_) {
        auto pinned_result = allocate_pinned_buffers();
        if (!pinned_result.is_ok()) {
            return pinned_result;
        }
    }

    Logger::instance().info("Initialization finished");
    return Result<bool>(true);
}

Result<bool> PipelineInitializer::initialize_reference() {
    // Stage 1: Load reference (0.00 - 0.10)
    if (update_progress_fn_) {
        update_progress_fn_(0.0, "Loading reference genome");
    }

    reference_loader_ = std::make_unique<ReferenceLoader>(config_.reference_fasta);
    auto load_result = reference_loader_->load();
    if (!load_result.is_ok()) {
        return load_result;
    }

    if (update_progress_fn_) {
        update_progress_fn_(0.10, "Reference genome loaded");
    }

    // Stage 2: Build/load FM-index (0.10 - 0.20)
    if (update_progress_fn_) {
        update_progress_fn_(0.10, "Building FM-index");
    }

    if (!reference_loader_->is_indexed()) {
        auto index_result = reference_loader_->build_index();
        if (!index_result.is_ok()) {
            return index_result;
        }
    }

    if (update_progress_fn_) {
        update_progress_fn_(0.20, "FM-index ready");
    }

    fm_index_view_ = reference_loader_->get_fm_index();
    concatenated_reference_ = &reference_loader_->concatenated_sequence();
    chromosome_offsets_ = reference_loader_->chromosome_offsets();

    return Result<bool>(true);
}

Result<bool> PipelineInitializer::initialize_gpu() {
    if (update_progress_fn_) {
        update_progress_fn_(0.20, "Initializing GPU");
    }

    if (!cuda_driver_available()) {
        Logger::instance().warn("CUDA driver not detected. Falling back to CPU pipeline.");
        return Result<bool>(true);
    }

    Logger::instance().info("CUDA driver detected, querying devices");
    int device_count = 0;
    cudaError_t device_err = cudaGetDeviceCount(&device_count);

    if (device_err != cudaSuccess) {
        Logger::instance().warn(std::string("cudaGetDeviceCount failed: ") +
                                cudaGetErrorString(device_err) +
                                ". Falling back to CPU pipeline.");
        return Result<bool>(true);
    }

    if (device_count == 0) {
        Logger::instance().warn("No CUDA-capable device detected. Falling back to CPU pipeline.");
        return Result<bool>(true);
    }

    if (config_.gpu_device_id >= device_count) {
        Logger::instance().warn("Requested GPU ID " + std::to_string(config_.gpu_device_id) +
                                " exceeds detected devices (" + std::to_string(device_count) +
                                "). Falling back to CPU pipeline.");
        return Result<bool>(true);
    }

    Logger::instance().info("Detected " + std::to_string(device_count) + " CUDA device(s)");
    Logger::instance().info("Setting active GPU to device " + std::to_string(config_.gpu_device_id));

    cudaError_t err = cudaSetDevice(config_.gpu_device_id);
    if (err != cudaSuccess) {
        Logger::instance().warn(std::string("Failed to set GPU device: ") +
                                cudaGetErrorString(err) +
                                ". Falling back to CPU pipeline.");
        return Result<bool>(true);
    }

    cudaDeviceProp prop;
    err = cudaGetDeviceProperties(&prop, config_.gpu_device_id);
    if (err != cudaSuccess) {
        Logger::instance().warn(std::string("Failed to query GPU properties: ") +
                                cudaGetErrorString(err) +
                                ". Falling back to CPU pipeline.");
        return Result<bool>(true);
    }

    std::cout << "Using GPU: " << prop.name << " (Compute "
              << prop.major << "." << prop.minor << ")\n";
    Logger::instance().info(std::string("Using GPU: ") + prop.name);

    // Initialize memory manager
    gpu_mem_manager_ = std::make_unique<cuda::MemoryManager>(config_.gpu_device_id);
    cudaError_t mem_err = gpu_mem_manager_->initialize();
    if (mem_err != cudaSuccess) {
        auto message = std::string("Failed to initialize GPU memory: ") +
                       cudaGetErrorString(mem_err);
        Logger::instance().error(message);
        return Result<bool>(ErrorCode::CUDA_ERROR, message);
    }

    // Allocate read batch buffers
    uint32_t max_reads_per_batch = config_.batch_size *
        (config_.read2_fastq.empty() ? 1u : 2u);

    Logger::instance().info("Allocating read batch buffers (" +
            std::to_string(max_reads_per_batch) + " reads)");
    cudaError_t alloc_err = cuda::allocate_read_batch(
        d_read_batch_, max_reads_per_batch, MAX_READ_LENGTH);
    if (alloc_err != cudaSuccess) {
        auto message = std::string("Failed to allocate read batch: ") +
                       cudaGetErrorString(alloc_err);
        Logger::instance().error(message);
        return Result<bool>(ErrorCode::CUDA_ERROR, message);
    }

    // Allocate seed buffer
    uint32_t max_seeds = max_reads_per_batch * (MAX_READ_LENGTH - config_.kmer_size + 1);
    Logger::instance().info("Allocating seed buffer for up to " +
            std::to_string(max_seeds) + " seeds");
    auto seed_err = gpu_mem_manager_->allocate(
        max_seeds * sizeof(cuda::Seed),
        (void**)&d_seeds_);
    if (seed_err != cudaSuccess) {
        auto message = std::string("Failed to allocate seed buffer: ") +
                       cudaGetErrorString(seed_err);
        Logger::instance().error(message);
        return Result<bool>(ErrorCode::CUDA_ERROR, message);
    }

    // Allocate alignment results buffer
    Logger::instance().info("Allocating alignment results buffer");
    auto res_err = cuda::allocate_alignment_results(
        d_results_, max_seeds, MAX_CIGAR_LENGTH);
    if (res_err != cudaSuccess) {
        auto message = std::string("Failed to allocate alignment results: ") +
                       cudaGetErrorString(res_err);
        Logger::instance().error(message);
        return Result<bool>(ErrorCode::CUDA_ERROR, message);
    }

    // Create compute stream
    if (!compute_stream_) {
        cudaStreamCreate(&compute_stream_);
    }

    // Copy FM-index to device
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
    }

    // Copy reference to device
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

    // Initialize multi-stream GPU contexts
    gpu_context_manager_ = std::make_unique<GpuContextManager>(
        config_, gpu_mem_manager_.get(), true);
    if (!gpu_context_manager_->initialize()) {
        Logger::instance().warn("Failed to initialize multi-stream GPU contexts. Using single-stream fallback.");
    } else {
        Logger::instance().info("Multi-stream GPU scheduler initialized with " +
            std::to_string(NUM_GPU_CONTEXTS) + " contexts");
    }

    gpu_enabled_ = true;
    if (update_progress_fn_) {
        update_progress_fn_(0.25, "GPU initialized");
    }

    return Result<bool>(true);
}

Result<bool> PipelineInitializer::initialize_bam_writer() {
    std::map<std::string, uint64_t> ref_sequences;
    const auto& sequences = reference_loader_->get_sequences();
    for (const auto& kv : sequences) {
        ref_sequences[kv.first] = kv.second.sequence.length();
    }

    bam_writer_ = std::make_unique<BamWriter>(config_.output_bam, ref_sequences);
    auto writer_result = bam_writer_->open();
    if (!writer_result.is_ok()) {
        return writer_result;
    }

    return Result<bool>(true);
}

Result<bool> PipelineInitializer::allocate_pinned_buffers() {
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

    return Result<bool>(true);
}

void PipelineInitializer::cleanup() {
    // Cleanup is handled by unique_ptr destructors and CUDA resource cleanup
    // Additional explicit cleanup can be added here if needed
}

} // namespace internal
} // namespace winalign
