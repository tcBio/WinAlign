/**
 * @file pipeline_multistream.cpp
 * @brief Multi-stream GPU scheduler implementation
 *
 * Implements overlapped pipeline processing with multiple GPU contexts
 * for maximum throughput (Phase 1 + Phase 2).
 */

#include "pipeline_multistream.h"
#include "pipeline_metrics.h"
#include "pipeline_batch_helpers.h"
#include "winalign/logger.h"
#include "winalign/pipeline.h"
#include "winalign/bam_writer.h"
#include "winalign/fastq_parser.h"
#include "winalign/fastq_worker.h"
#include "winalign/cuda/memory_manager.cuh"
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>

namespace winalign {
namespace internal {

MultiStreamScheduler::MultiStreamScheduler(
    const PipelineConfig& config,
    std::vector<GpuBatchContext>& contexts,
    MetricsCollector& metrics,
    BatchProcessingHelpers& batch_helpers,
    std::atomic<bool>& cancelled)
    : config_(config)
    , gpu_contexts_(contexts)
    , metrics_(metrics)
    , batch_helpers_(batch_helpers)
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
                        } else if (ctx.read_count > 0) {
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
                if (check_event(ctx.event_h2d_done, ctx, all_contexts_done)) {
                    record_timing(ctx.event_h2d_done, ctx.event_h2d_done, ctx.timing.t_h2d);
                    cudaError_t err = launch_seeding(ctx);
                    ctx.state = (err == cudaSuccess) ? ContextState::SEEDING : ContextState::ALIGNING;
                    if (err != cudaSuccess) {
                        Logger::instance().warn("Seeding failed: " + std::string(cudaGetErrorString(err)));
                    }
                    all_contexts_done = false;
                }
            }
        }

        // === ALIGNMENT STAGE: Launch SW for contexts that finished seeding ===
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::SEEDING) {
                if (check_event(ctx.event_seeding_done, ctx, all_contexts_done)) {
                    record_timing(ctx.event_h2d_done, ctx.event_seeding_done, ctx.timing.t_gpu_seed);
                    cudaError_t err = launch_alignment(ctx);
                    ctx.state = (err == cudaSuccess) ? ContextState::ALIGNING : ContextState::COPYING_BACK;
                    if (err != cudaSuccess) {
                        Logger::instance().warn("Alignment failed");
                    }
                    all_contexts_done = false;
                }
            }
        }

        // === D2H TRANSFER STAGE: Launch async D2H for contexts that finished alignment ===
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::ALIGNING) {
                if (check_event(ctx.event_sw_done, ctx, all_contexts_done)) {
                    record_timing(ctx.event_seeding_done, ctx.event_sw_done, ctx.timing.t_gpu_align);
                    cudaError_t err = launch_d2h_transfer(ctx);
                    ctx.state = (err == cudaSuccess) ? ContextState::COPYING_BACK : ContextState::EMPTY;
                    if (err != cudaSuccess) {
                        Logger::instance().warn("D2H transfer failed");
                    }
                    all_contexts_done = false;
                }
            }
        }

        // === HOST PROCESSING STAGE: Process contexts that finished D2H ===
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::COPYING_BACK) {
                if (check_event(ctx.event_d2h_done, ctx, all_contexts_done)) {
                    record_timing(ctx.event_sw_done, ctx.event_d2h_done, ctx.timing.t_d2h);
                    ctx.state = ContextState::DONE;
                    all_contexts_done = false;
                }
            }

            if (ctx.state == ContextState::DONE) {
                // Process results (aggregate, write BAM)
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

        all_contexts_done = parser_eof && std::all_of(gpu_contexts_.begin(), gpu_contexts_.end(),
            [](const auto& c) { return c.state == ContextState::EMPTY; });

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

    // Generate all seeds
    cudaError_t err = cuda::generate_gpu_seeds(
        ctx.d_read_batch,
        d_fm_index_,
        ctx.d_seeds,
        MAX_SEEDS_PER_READ,
        config_.kmer_size,
        step,
        ctx.num_seeds,
        ctx.stream);

    if (err != cudaSuccess) return err;

    // Build per-read offsets and counts for chaining
    // This is done on CPU for now (could be optimized to GPU later)
    std::vector<uint32_t> seeds_per_read_offsets(ctx.read_count);
    std::vector<uint32_t> seeds_per_read_counts(ctx.read_count, 0);

    // Copy seeds to host temporarily to count per-read
    std::vector<cuda::Seed> host_seeds_temp(ctx.num_seeds);
    err = cudaMemcpyAsync(host_seeds_temp.data(), ctx.d_seeds,
                         ctx.num_seeds * sizeof(cuda::Seed),
                         cudaMemcpyDeviceToHost, ctx.stream);
    if (err != cudaSuccess) return err;
    cudaStreamSynchronize(ctx.stream);

    // Count seeds per read
    for (uint32_t i = 0; i < ctx.num_seeds; i++) {
        seeds_per_read_counts[host_seeds_temp[i].read_id]++;
    }

    // Calculate offsets (cumulative sum)
    seeds_per_read_offsets[0] = 0;
    for (uint32_t i = 1; i < ctx.read_count; i++) {
        seeds_per_read_offsets[i] = seeds_per_read_offsets[i-1] + seeds_per_read_counts[i-1];
    }

    // Copy to device
    err = cudaMemcpyAsync(ctx.d_seeds_per_read_offsets, seeds_per_read_offsets.data(),
                         ctx.read_count * sizeof(uint32_t),
                         cudaMemcpyHostToDevice, ctx.stream);
    if (err != cudaSuccess) return err;

    err = cudaMemcpyAsync(ctx.d_seeds_per_read_counts, seeds_per_read_counts.data(),
                         ctx.read_count * sizeof(uint32_t),
                         cudaMemcpyHostToDevice, ctx.stream);
    if (err != cudaSuccess) return err;

    // Chain seeds to find best seed per read (3-5x speedup)
    err = cuda::chain_seeds(
        ctx.d_seeds,
        ctx.d_seeds_per_read_offsets,
        ctx.d_seeds_per_read_counts,
        ctx.read_count,
        ctx.d_best_seeds,
        ctx.d_chain_scores,
        ctx.stream);

    if (err == cudaSuccess) {
        cudaEventRecord(ctx.event_seeding_done, ctx.stream);
    }

    return err;
}

cudaError_t MultiStreamScheduler::launch_alignment(GpuBatchContext& ctx) {
    // FIX: With seed chaining, check read_count (not num_seeds)
    if (ctx.read_count == 0) {
        return cudaSuccess;
    }

    cuda::SWParams sw_params;
    sw_params.match_score = config_.scores.match;
    sw_params.mismatch_score = config_.scores.mismatch;
    sw_params.gap_open = config_.scores.gap_open;
    sw_params.gap_extend = config_.scores.gap_extend;

    // OPTIMIZATION: Use chained best seeds instead of all seeds
    // This reduces alignments from 10-50 per read to 1 per read (3-5x speedup)
    cudaError_t err = cuda::smith_waterman_align(
        ctx.d_read_batch,
        ctx.d_best_seeds,      // CHANGED: Use best seeds from chaining
        ctx.read_count,        // CHANGED: One alignment per read instead of num_seeds
        d_reference_,
        reference_length_,
        sw_params,
        ctx.d_results,
        ctx.stream);
    if (err != cudaSuccess) return err;

    err = cuda::calculate_mapping_quality(
        ctx.d_results, ctx.read_count, ctx.stream);  // CHANGED: read_count instead of num_seeds
    if (err != cudaSuccess) return err;

    cudaEventRecord(ctx.event_sw_done, ctx.stream);

    return cudaSuccess;
}

cudaError_t MultiStreamScheduler::launch_d2h_transfer(GpuBatchContext& ctx) {
    if (ctx.num_seeds == 0) {
        cudaEventRecord(ctx.event_d2h_done, ctx.stream);
        return cudaSuccess;
    }

    // OPTIMIZATION: Transfer only read_count results (1 per read) instead of num_seeds
    size_t bytes = ctx.read_count * sizeof(cuda::AlignmentResult);

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

// Helper: Check event and handle errors, returns true if event complete
bool MultiStreamScheduler::check_event(cudaEvent_t event, GpuBatchContext& ctx, bool& all_done) {
    cudaError_t err = cudaEventQuery(event);
    if (err == cudaSuccess) return true;
    if (err != cudaErrorNotReady) {
        Logger::instance().error("Event error context " + std::to_string(ctx.context_id));
        ctx.state = ContextState::EMPTY;
    } else {
        all_done = false;
    }
    return false;
}

// Helper: Record timing between two events
void MultiStreamScheduler::record_timing(cudaEvent_t start, cudaEvent_t end, double& timing) {
    float ms = 0.0f;
    cudaEventElapsedTime(&ms, start, end);
    timing = ms;
}

void MultiStreamScheduler::process_context_results(GpuBatchContext& ctx) {
    using namespace std::chrono;
    auto write_start = high_resolution_clock::now();

    // Ensure D2H transfer is fully complete before accessing pinned memory
    if (ctx.stream) {
        cudaStreamSynchronize(ctx.stream);
    }

    // Copy GPU results to host
    // OPTIMIZATION: With seed chaining, we only have read_count results (1 per read)
    ctx.host_results.resize(ctx.read_count);
    if (ctx.read_count > 0 && ctx.pinned_results) {
        std::memcpy(ctx.host_results.data(), ctx.pinned_results,
                   ctx.read_count * sizeof(cuda::AlignmentResult));
    }

    // Build alignments and write to BAM
    for (size_t i = 0; i < ctx.read_count; ++i) {
        const auto& view = ctx.read_views[i];

        // OPTIMIZATION: With seed chaining, result index == read index (no search needed)
        const cuda::AlignmentResult& result = ctx.host_results[i];
        bool found = (result.score > 0);

        Alignment aln;
        const Read* current_read_ptr = nullptr;

        if (found) {
            // Build alignment from GPU result
            aln = batch_helpers_.build_alignment_from_gpu_result(result, view);
            batch_helpers_.update_metrics(true, aln.mapq);
            current_read_ptr = view.read;
        } else {
            // Build unmapped alignment
            // Get the actual read (handle paired vs single-end)
            bool is_second = false;

            if (ctx.is_paired) {
                size_t pair_idx = i / 2;
                is_second = (i % 2 == 1);
                current_read_ptr = is_second ? &ctx.host_pairs[pair_idx].read2
                                             : &ctx.host_pairs[pair_idx].read1;
            } else {
                current_read_ptr = &ctx.host_singles[i];
            }

            aln = batch_helpers_.build_unmapped_alignment(*current_read_ptr, ctx.is_paired, is_second);
            batch_helpers_.update_metrics(false, 0);
        }

        // Write to BAM with thread safety and error handling
        if (bam_writer_ && current_read_ptr) {
            std::lock_guard<std::mutex> lock(bam_write_mutex_);
            Result<bool> write_result = bam_writer_->write(aln, *current_read_ptr);
            if (!write_result.is_ok()) {
                Logger::instance().error("Failed to write alignment for read " +
                                        current_read_ptr->name + ": " +
                                        write_result.error_message());
            }
        }
    }

    ctx.timing.t_write = duration_cast<microseconds>(
        high_resolution_clock::now() - write_start).count() / 1000.0;

    PerformanceCounters counters;
    counters.total_gpu_seeds = ctx.num_seeds;
    counters.batches_processed = 1;
    metrics_.update_counters(counters);

    ctx.timing.t_total = duration_cast<microseconds>(
        high_resolution_clock::now() - ctx.batch_start).count() / 1000.0;
    metrics_.record_batch(ctx.timing, metrics_.get_batch_count() + 1);
}

} // namespace internal
} // namespace winalign
