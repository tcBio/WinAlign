# WinAlign Performance Optimization - Implementation Progress

## Overview
This document tracks the implementation of performance optimizations for the WinAlign GPU aligner, targeting throughput improvements to compete with Parabricks on RTX 5090/5070 hardware.

---

## ✅ Phase 0: Baseline Profiling & Timing Hooks (COMPLETE)

### Implemented Features

1. **Per-Batch Timing Instrumentation**
   - Added `BatchTiming` structure tracking:
     - `t_h2d`: Host-to-device transfer time
     - `t_gpu_seed`: GPU seeding kernel time
     - `t_gpu_align`: GPU Smith-Waterman alignment time
     - `t_d2h`: Device-to-host transfer time
     - `t_write`: BAM write time
     - `t_total`: Total batch processing time

2. **CUDA Event-Based GPU Timing**
   - Created CUDA events for precise GPU kernel timing:
     - `event_h2d_start/done`
     - `event_seeding_start/done`
     - `event_sw_start/done`
     - `event_d2h_start/done`
   - Instrumented all kernel launches with `cudaEventRecord()`
   - Events properly created in `initialize()` and destroyed in `finalize()`

3. **Performance Counters**
   - `PerformanceCounters` structure tracking:
     - `gpu_aligned_reads`: Successfully GPU-aligned reads
     - `unmapped_reads`: Reads that failed to align
     - `reads_without_seeds`: Reads with no seeds found
     - `total_gpu_seeds`: Cumulative GPU seeds generated
     - `batches_processed`: Total batches processed
   - Counters updated in real-time during batch processing

4. **Logging and Reporting**
   - `log_batch_timing()`: Logs detailed timing per batch
   - `log_performance_summary()`: Comprehensive summary at pipeline end
   - Reports average timings across all batches
   - Provides seeds-per-read statistics

### Code Locations
- **File**: [pipeline.cpp:705-813](c:/local/ins/WinAlign/src/core/pipeline.cpp#L705-L813)
- **Timing Instrumentation**: Lines 820-1142 (process_gpu_batch)
- **Summary Output**: finalize() method calls log_performance_summary()

### Example Output
```
Batch 1 timing: read=0.0ms, H2D=12.3ms, seed=45.2ms, align=123.5ms, D2H=8.1ms, write=15.3ms, total=204.4ms
Batch 2 timing: read=0.0ms, H2D=11.9ms, seed=44.8ms, align=121.2ms, D2H=7.9ms, write=14.8ms, total=200.6ms
...
=== Performance Summary ===
Total batches: 17
GPU aligned reads: 850000
Unmapped reads: 150000
Reads without seeds: 25000
Total GPU seeds: 34000000
Avg seeds/read: 40.0
=== Cumulative Timing (avg per batch) ===
Read: 0.0 ms
H2D: 12.1 ms
GPU Seed: 45.0 ms
GPU Align: 122.3 ms
D2H: 8.0 ms
Write: 15.1 ms
Total: 202.5 ms
```

### Benefits
- Establishes baseline performance metrics
- Identifies bottlenecks (e.g., alignment kernel dominating time)
- Enables before/after comparisons for future optimizations
- Provides visibility into GPU vs CPU balance

---

## 🚧 Phase 1: Multi-Stream GPU Scheduler (IN PROGRESS)

### Implemented Features

1. **GpuBatchContext Structure** ✅
   - State machine with 9 states:
     - `EMPTY`, `LOADING`, `READY`, `TRANSFERRING`
     - `SEEDING`, `ALIGNING`, `COPYING_BACK`, `DONE`, `PROCESSING`
   - Per-context resources:
     - Dedicated `cudaStream_t` for async operations
     - Independent device buffers (reads, seeds, results)
     - Pinned host buffers for faster transfers
     - CUDA events for sync points
   - Location: [pipeline.cpp:819-876](c:/local/ins/WinAlign/src/core/pipeline.cpp#L819-L876)

2. **Context Initialization** ✅
   - `initialize_gpu_contexts()` allocates 3 concurrent contexts
   - Each context gets:
     - Dedicated CUDA stream
     - 4 CUDA events (H2D, seeding, SW, D2H)
     - Device buffers (read batch, seeds, results)
     - Pinned host buffers (all transfer buffers)
   - Location: [pipeline.cpp:882-957](c:/local/ins/WinAlign/src/core/pipeline.cpp#L882-L957)

3. **Context Cleanup** ✅
   - `cleanup_gpu_contexts()` properly releases all resources
   - Synchronizes streams before destruction
   - Frees all device and pinned memory
   - Location: [pipeline.cpp:959-986](c:/local/ins/WinAlign/src/core/pipeline.cpp#L959-L986)

4. **Integration Points** ✅
   - Called in `initialize()`: [pipeline.cpp:291-298](c:/local/ins/WinAlign/src/core/pipeline.cpp#L291-L298)
   - Called in `finalize()`: [pipeline.cpp:570-571](c:/local/ins/WinAlign/src/core/pipeline.cpp#L570-L571)

### Remaining Work

#### 1. Refactor `run()` into Scheduler Loop
**Current**: Single-stream synchronous batch processing
**Target**: Event-driven async multi-batch pipeline

**Pseudocode for New Scheduler**:
```cpp
Result<bool> run_multi_stream() {
    // Open FASTQ parsers
    auto parser = open_fastq_parsers();

    int load_idx = 0;   // Which context to load next
    int launch_idx = 0; // Which context to launch kernels on
    int write_idx = 0;  // Which context to write BAM from

    bool all_done = false;

    while (!cancelled_ && !all_done) {
        all_done = true;  // Assume done unless we find work

        // === LOAD STAGE ===
        // Try to fill an EMPTY context with new FASTQ data
        for (int i = 0; i < NUM_GPU_CONTEXTS; ++i) {
            auto& ctx = gpu_contexts_[i];
            if (ctx.state == ContextState::EMPTY && !parser_eof) {
                // Read FASTQ batch
                read_fastq_batch(ctx, parser);

                // Prepare for GPU (flatten to pinned buffers)
                prepare_context_for_gpu(ctx);

                ctx.state = ContextState::READY;
                all_done = false;
            }
        }

        // === H2D TRANSFER STAGE ===
        // Launch async H2D transfers for READY contexts
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::READY) {
                cudaEventRecord(ctx.event_h2d_start, ctx.stream);

                // Async copy pinned → device
                copy_reads_to_device_async(ctx);

                cudaEventRecord(ctx.event_h2d_done, ctx.stream);
                ctx.state = ContextState::TRANSFERRING;
                all_done = false;
            }
        }

        // === SEEDING STAGE ===
        // Launch seeding for contexts that finished H2D
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::TRANSFERRING) {
                cudaError_t err = cudaEventQuery(ctx.event_h2d_done);
                if (err == cudaSuccess) {
                    // H2D complete, launch seeding
                    cudaEventRecord(ctx.event_seeding_start, ctx.stream);

                    cuda::generate_gpu_seeds(
                        ctx.d_read_batch, d_fm_index_, ctx.d_seeds,
                        /*...*/, ctx.stream);

                    cudaEventRecord(ctx.event_seeding_done, ctx.stream);
                    ctx.state = ContextState::SEEDING;
                    all_done = false;
                }
            }
        }

        // === ALIGNMENT STAGE ===
        // Launch SW for contexts that finished seeding
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::SEEDING) {
                cudaError_t err = cudaEventQuery(ctx.event_seeding_done);
                if (err == cudaSuccess) {
                    // Seeding complete, launch alignment
                    cudaEventRecord(ctx.event_sw_start, ctx.stream);

                    cuda::smith_waterman_align(
                        ctx.d_read_batch, ctx.d_seeds, /*...*/, ctx.stream);
                    cuda::calculate_mapping_quality(
                        ctx.d_results, /*...*/, ctx.stream);

                    cudaEventRecord(ctx.event_sw_done, ctx.stream);
                    ctx.state = ContextState::ALIGNING;
                    all_done = false;
                }
            }
        }

        // === D2H TRANSFER STAGE ===
        // Launch async D2H for contexts that finished alignment
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::ALIGNING) {
                cudaError_t err = cudaEventQuery(ctx.event_sw_done);
                if (err == cudaSuccess) {
                    // SW complete, copy results back
                    cudaEventRecord(ctx.event_d2h_start, ctx.stream);

                    copy_results_to_host_async(ctx);

                    cudaEventRecord(ctx.event_d2h_done, ctx.stream);
                    ctx.state = ContextState::COPYING_BACK;
                    all_done = false;
                }
            }
        }

        // === HOST PROCESSING STAGE ===
        // Process contexts that finished D2H
        for (auto& ctx : gpu_contexts_) {
            if (ctx.state == ContextState::COPYING_BACK) {
                cudaError_t err = cudaEventQuery(ctx.event_d2h_done);
                if (err == cudaSuccess) {
                    // D2H complete, results ready
                    ctx.state = ContextState::DONE;
                    all_done = false;
                }
            }

            if (ctx.state == ContextState::DONE) {
                // Do host-side work (aggregate, write BAM)
                process_context_results(ctx);

                // Mark context as EMPTY for reuse
                ctx.state = ContextState::EMPTY;
                all_done = false;
            }
        }

        // Small sleep to avoid busy-waiting (optional)
        // std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    // Final synchronization - wait for all contexts to finish
    for (auto& ctx : gpu_contexts_) {
        if (ctx.stream) {
            cudaStreamSynchronize(ctx.stream);
        }
    }

    return Result<bool>(true);
}
```

**Key Benefits**:
- **Overlap**: While context 0 is seeding, context 1 transfers H2D, context 2 writes BAM
- **GPU Utilization**: GPU never idles waiting for CPU
- **Throughput**: 2-3x speedup expected from pipelining alone

#### 2. Helper Functions to Implement

```cpp
// Read FASTQ batch into context
void read_fastq_batch(GpuBatchContext& ctx, FastqParser& parser) {
    ctx.host_pairs.clear();
    ctx.host_singles.clear();
    ctx.read_count = parser.next_batch(ctx.host_singles, config_.batch_size);
    ctx.is_paired = false; // or handle paired-end
    ctx.eof = (ctx.read_count == 0);
}

// Flatten reads into pinned buffers
void prepare_context_for_gpu(GpuBatchContext& ctx) {
    ctx.host_read_sequences.clear();
    ctx.host_read_offsets.clear();
    ctx.host_read_lengths.clear();
    ctx.read_views.clear();

    for (const auto& read : ctx.host_singles) {
        ReadView view;
        view.read = &read;
        ctx.read_views.push_back(view);

        uint32_t offset = ctx.host_read_sequences.size();
        ctx.host_read_offsets.push_back(offset);
        ctx.host_read_lengths.push_back(read.sequence.size());

        ctx.host_read_sequences.insert(
            ctx.host_read_sequences.end(),
            read.sequence.begin(), read.sequence.end());
    }

    // Copy to pinned buffers
    memcpy(ctx.pinned_sequences, ctx.host_read_sequences.data(), ...);
    memcpy(ctx.pinned_offsets, ctx.host_read_offsets.data(), ...);
    memcpy(ctx.pinned_lengths, ctx.host_read_lengths.data(), ...);
}

// Async H2D transfer
void copy_reads_to_device_async(GpuBatchContext& ctx) {
    cudaMemcpyAsync(ctx.d_read_batch.sequences, ctx.pinned_sequences,
                    ..., cudaMemcpyHostToDevice, ctx.stream);
    cudaMemcpyAsync(ctx.d_read_batch.offsets, ctx.pinned_offsets,
                    ..., cudaMemcpyHostToDevice, ctx.stream);
    cudaMemcpyAsync(ctx.d_read_batch.lengths, ctx.pinned_lengths,
                    ..., cudaMemcpyHostToDevice, ctx.stream);
}

// Async D2H transfer
void copy_results_to_host_async(GpuBatchContext& ctx) {
    size_t bytes = ctx.num_seeds * sizeof(cuda::AlignmentResult);
    cudaMemcpyAsync(ctx.pinned_results, ctx.d_results, bytes,
                    cudaMemcpyDeviceToHost, ctx.stream);
}

// Host-side result processing
void process_context_results(GpuBatchContext& ctx) {
    // Copy from pinned to host buffers
    ctx.host_results.resize(ctx.num_seeds);
    memcpy(ctx.host_results.data(), ctx.pinned_results, ...);

    // Find best alignment per read
    std::vector<BestResult> best_per_read = select_best_alignments(ctx);

    // Write to BAM
    write_alignments_to_bam(ctx, best_per_read);

    // Update metrics
    update_performance_counters(ctx);
}
```

#### 3. Integration Strategy

**Option A: Replace current `run()` entirely**
- Rename current `run()` to `run_single_stream()`
- Implement new `run_multi_stream()`
- Add config flag: `use_multi_stream_scheduler`
- Call appropriate version based on flag

**Option B: Hybrid approach**
- Keep single-stream as fallback
- Auto-detect: if multi-stream init succeeds, use it
- Log which mode is active

### Expected Performance Gains

**Current (Single-Stream)**:
```
Batch N:   [Load] [H2D] [Seed] [SW] [D2H] [Write]
Batch N+1:                                          [Load] [H2D] [Seed] [SW] [D2H] [Write]
```
- GPU idle during Load and Write
- Total time: sum of all stages

**Multi-Stream (3 contexts)**:
```
Ctx 0:  [Load] [H2D] [Seed] [SW] [D2H] [Write]
Ctx 1:         [Load] [H2D] [Seed] [SW] [D2H] [Write]
Ctx 2:                [Load] [H2D] [Seed] [SW] [D2H] [Write]
```
- GPU continuously busy
- Total throughput: limited by slowest stage (likely SW kernel)
- **Estimated speedup**: 2-3x for alignment stage

---

## 📋 Phases 2-6: Roadmap

### Phase 2: Multi-threaded FASTQ IO
- **Goal**: Parallelize gzip decompression and parsing
- **Approach**:
  - Create worker thread pool
  - Pre-chunk FASTQ files
  - Workers feed into context queue
- **Expected gain**: 1.5-2x for large gzipped files

### Phase 3: Warp-Optimized Banded SW
- **Goal**: Faster alignment kernel
- **Approach**:
  - One warp per alignment (32 threads collaborate)
  - Banded DP (±64 around seed)
  - Shared memory for band
  - Warp intrinsics (`__shfl_sync`)
- **Expected gain**: 3-5x for alignment stage

### Phase 4: GPU Seeding Optimizations
- **Goal**: Reduce seeding time
- **Approach**:
  - Preload BWT chunks into shared memory
  - 2-bit sequence encoding
  - Filter repetitive seeds on GPU
- **Expected gain**: 1.5-2x for seeding stage

### Phase 5: Multi-Process Orchestration
- **Goal**: Scale across all CPU cores
- **Approach**:
  - `winalign-chunker`: Split FASTQ into chunks
  - `winalign-runner`: Launch parallel processes
  - `samtools merge`: Combine outputs
- **Expected gain**: Near-linear with core count

### Phase 6: Validation & Tuning
- **Goal**: Match Parabricks accuracy and speed
- **Approach**:
  - Compare alignment results vs Parabricks
  - Tune batch size, band width, etc.
  - Stability testing on real samples

---

## 🎯 Next Steps

### Immediate (Complete Phase 1)
1. Implement `run_multi_stream()` scheduler loop
2. Add helper functions for context management
3. Test with small FASTQ sample
4. Validate correctness vs single-stream mode
5. Benchmark throughput improvement

### Testing Strategy
```bash
# Build with Phase 1
cmake --build build --config Release

# Run on small test data
./build/bin/Release/winalign-gpu \
    --reference test/data/ref.fa \
    --read1 test/data/reads_1M.fastq.gz \
    --output test.bam \
    --batch-size 60000

# Compare outputs
samtools view test.bam | head -100
```

### Performance Validation
- Compare log output before/after Phase 1
- Look for:
  - Reduced total batch time
  - Better GPU utilization (less idle time)
  - Higher reads/sec throughput

---

## 📊 Current Status Summary

| Phase | Status | Completion | Estimated Speedup |
|-------|--------|------------|-------------------|
| Phase 0: Profiling | ✅ Complete | 100% | Baseline |
| Phase 1: Multi-stream | 🚧 60% | 60% | 2-3x |
| Phase 2: MT FASTQ IO | ⏳ Not started | 0% | 1.5-2x |
| Phase 3: Warp SW | ⏳ Not started | 0% | 3-5x |
| Phase 4: Seeding | ⏳ Not started | 0% | 1.5-2x |
| Phase 5: Multi-process | ⏳ Not started | 0% | Linear with cores |
| Phase 6: Tuning | ⏳ Not started | 0% | - |

**Combined Theoretical Speedup**: 10-30x (compounding gains)

---

## 📝 Notes

- All code changes are in: `c:/local/ins/WinAlign/src/core/pipeline.cpp`
- Baseline profiling is fully functional and logging
- Multi-stream infrastructure is set up but not yet driving the pipeline
- Original single-stream code path still active (can be kept as fallback)

**Author**: Claude Code (Anthropic)
**Last Updated**: 2025-11-16
