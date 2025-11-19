/**
 * @file pipeline_multistream.cpp
 * @brief Multi-stream GPU scheduler implementation
 *
 * Implements overlapped pipeline processing with multiple GPU contexts
 * for maximum throughput (Phase 1 + Phase 2).
 */

#include "pipeline_multistream.h"
#include "pipeline_metrics.h"
#include "winalign/logger.h"
#include "winalign/pipeline.h"
#include "winalign/bam_writer.h"
#include "winalign/fastq_parser.h"
#include "winalign/fastq_worker.h"
#include "winalign/cuda/memory_manager.cuh"
#include <chrono>
#include <thread>
#include <cstring>

namespace winalign {
namespace internal {

MultiStreamScheduler::MultiStreamScheduler(
    const PipelineConfig& config,
    std::vector<GpuBatchContext>& contexts,
    MetricsCollector& metrics,
    std::atomic<bool>& cancelled)
    : config_(config)
    , gpu_contexts_(contexts)
    , metrics_(metrics)
    , cancelled_(cancelled)
    , d_fm_index_(nullptr)
    , d_reference_(nullptr)
    , reference_length_(0)
    , bam_writer_(nullptr)
    , estimated_total_reads_(0)
{
}

bool MultiStreamScheduler::initialize(
    const cuda::FMIndexView* d_fm_index,
    const char* d_reference,
    size_t reference_length,
    BamWriter* bam_writer,
    uint64_t estimate_total_reads,
    std::function<void(double, const std::string&)> update_progress_fn)
{
    d_fm_index_ = d_fm_index;
    d_reference_ = d_reference;
    reference_length_ = reference_length;
    bam_writer_ = bam_writer;
    estimated_total_reads_ = estimate_total_reads;
    update_progress_fn_ = update_progress_fn;
    return true;
}

Result<bool> MultiStreamScheduler::run() {
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
        return start_result;
    }

    Logger::instance().info("FASTQ worker started with " + std::to_string(num_io_workers) +
                           " IO threads");

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
                        load_fastq_into_context_from_worker(ctx,
                            std::move(batch.pairs),
                            std::move(batch.singles),
                            batch.count,
                            batch.is_paired,
                            batch.eof);

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
                    cudaEventElapsedTime(&h2d_time, ctx.event_h2d_done, ctx.event_h2d_done);
                    ctx.timing.t_h2d = h2d_time;

                    // Launch seeding
                    err = launch_seeding(ctx);
                    if (err != cudaSuccess) {
                        Logger::instance().warn("GPU seeding failed for context " +
                            std::to_string(ctx.context_id) + ": " + cudaGetErrorString(err));
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
                if (update_progress_fn_) {
                    double alignment_progress = 0.25 + (0.65 *
                        static_cast<double>(processed_reads) / estimated_total_reads_);
                    alignment_progress = std::min(alignment_progress, 0.90);
                    update_progress_fn_(alignment_progress,
                                       "Aligning reads: " + std::to_string(processed_reads) + "/" +
                                       std::to_string(estimated_total_reads_));
                }

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

        // Small sleep to avoid busy-waiting
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

    if (update_progress_fn_) {
        update_progress_fn_(0.90, "Multi-stream alignment complete");
    }
    Logger::instance().info("Multi-stream alignment loop finished: " +
                           std::to_string(processed_reads) + " reads processed");

    return Result<bool>(true);
}

// === Helper Functions Implementation ===

void MultiStreamScheduler::load_fastq_into_context_from_worker(
    GpuBatchContext& ctx,
    std::vector<ReadPair>&& pairs,
    std::vector<Read>&& singles,
    size_t count,
    bool is_paired,
    bool eof)
{
    using namespace std::chrono;
    auto read_start = high_resolution_clock::now();

    ctx.host_pairs = std::move(pairs);
    ctx.host_singles = std::move(singles);
    ctx.read_count = count;
    ctx.is_paired = is_paired;
    ctx.eof = eof;

    auto read_end = high_resolution_clock::now();
    ctx.timing.t_read = duration_cast<microseconds>(read_end - read_start).count() / 1000.0;
}

bool MultiStreamScheduler::prepare_context_for_gpu(GpuBatchContext& ctx) {
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
    if (!ctx.read_views.empty() && ctx.pinned_sequences &&
        ctx.pinned_offsets && ctx.pinned_lengths) {
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

cudaError_t MultiStreamScheduler::launch_h2d_transfer(GpuBatchContext& ctx) {
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

cudaError_t MultiStreamScheduler::launch_seeding(GpuBatchContext& ctx) {
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

cudaError_t MultiStreamScheduler::launch_alignment(GpuBatchContext& ctx) {
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

cudaError_t MultiStreamScheduler::launch_d2h_transfer(GpuBatchContext& ctx) {
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

void MultiStreamScheduler::process_context_results(GpuBatchContext& ctx) {
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

    // TODO: Implement alignment building (requires pipeline helper methods)
    // For now, this is a placeholder
    Logger::instance().warn("Alignment building not yet implemented in MultiStreamScheduler");

    if (!alignments.empty() && bam_writer_) {
        bam_writer_->write_batch(alignments, output_reads);
    }

    auto write_end = high_resolution_clock::now();
    ctx.timing.t_write = duration_cast<microseconds>(write_end - write_start).count() / 1000.0;

    // Update performance counters
    PerformanceCounters counters;
    counters.total_gpu_seeds = ctx.num_seeds;
    counters.batches_processed = 1;
    metrics_.update_counters(counters);

    // Calculate total batch time
    auto batch_end = high_resolution_clock::now();
    ctx.timing.t_total = duration_cast<microseconds>(batch_end - ctx.batch_start).count() / 1000.0;

    // Record batch timing
    metrics_.record_batch(ctx.timing, metrics_.get_batch_count() + 1);
}

} // namespace internal
} // namespace winalign
