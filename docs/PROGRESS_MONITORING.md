# Progress Monitoring in WinAlign-GPU

## Overview

WinAlign-GPU implements comprehensive progress monitoring throughout the entire alignment pipeline, providing real-time feedback on the processing status. This is crucial for long-running genomic alignments where users need visibility into the pipeline's progress.

## Progress Stages

The pipeline is divided into 5 main stages, each with a defined percentage of total progress:

### Stage 1: Loading Reference (0% - 10%)
- **Operation**: Load reference genome from FASTA file
- **Activities**:
  - Open and parse FASTA file
  - Validate sequences
  - Load into memory structures
- **Typical Duration**: 30 seconds - 2 minutes (depending on genome size)

### Stage 2: Building FM-Index (10% - 20%)
- **Operation**: Construct or load FM-index for seed matching
- **Activities**:
  - Build Burrows-Wheeler Transform (BWT)
  - Construct suffix array
  - Create occurrence tables
  - Or load pre-built index from disk
- **Typical Duration**: 5 - 15 minutes (first time) or 10-30 seconds (cached)

### Stage 3: GPU Initialization (20% - 25%)
- **Operation**: Initialize CUDA device and allocate GPU memory
- **Activities**:
  - Select and configure GPU device
  - Allocate device memory for reference index
  - Transfer FM-index to GPU
  - Allocate read batch buffers
- **Typical Duration**: 5-15 seconds

### Stage 4: Alignment (25% - 90%)
- **Operation**: Main alignment processing loop
- **Activities**:
  - Parse FASTQ reads in batches
  - Transfer reads to GPU
  - GPU seeding (k-mer extraction and matching)
  - GPU alignment (Smith-Waterman)
  - GPU filtering (quality, duplicates, pairs)
  - Transfer results back to host
  - Write to output file
- **Typical Duration**: 20-40 minutes (30x WGS on RTX 5090)
- **Sub-Progress**: Updated continuously as batches are processed

### Stage 5: Finalization (90% - 100%)
- **Operation**: Finalize output and generate metrics
- **Activities**:
  - Sort BAM file (if requested) - 92%
  - Mark duplicates (if requested) - 95%
  - Generate BAM index - 97%
  - Write QC metrics to JSON - 99%
  - Cleanup resources - 100%
- **Typical Duration**: 1-3 minutes

## Progress Callback Interface

### Setting Up Progress Monitoring

```cpp
Pipeline pipeline(config);

// Set progress callback
pipeline.set_progress_callback([](double progress) {
    // progress ranges from 0.0 to 1.0
    std::cout << "\rProgress: " << static_cast<int>(progress * 100) << "%";
    std::cout << std::flush;
});
```

### Enhanced Progress Display Example

```cpp
pipeline.set_progress_callback([](double progress) {
    // Create a progress bar
    const int bar_width = 50;
    int filled = static_cast<int>(progress * bar_width);

    std::cout << "\r[";
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled) std::cout << "=";
        else if (i == filled) std::cout << ">";
        else std::cout << " ";
    }
    std::cout << "] " << static_cast<int>(progress * 100) << "% ";
    std::cout << std::flush;
});
```

Output:
```
[================================>                 ] 65%
```

## Internal Progress Tracking

### Progress Update Function

The pipeline internally uses `update_progress()` to report progress:

```cpp
void update_progress(double progress, const std::string& stage_info = "") {
    current_progress_ = progress;
    current_stage_info_ = stage_info;

    if (progress_callback_) {
        progress_callback_(progress);
    }
}
```

### Progress Calculation in Alignment Stage

The alignment stage (25% - 90%) calculates progress based on reads processed:

```cpp
// Calculate alignment stage progress (0.25 to 0.90)
double alignment_progress = 0.25 + (0.65 * processed_reads / estimated_total_reads);
alignment_progress = std::min(alignment_progress, 0.90);

update_progress(alignment_progress,
               "Aligning reads: " + std::to_string(processed_reads) + "/" +
               std::to_string(estimated_total_reads));
```

## Performance Metrics Integration

Progress monitoring is integrated with performance tracking:

```cpp
// During alignment
metrics_.total_reads += batch_count;
metrics_.aligned_reads += aligned_count;

// Progress reflects actual work completed
double work_completed = metrics_.total_reads / estimated_total_reads;
```

## Cancellation Support

The progress system supports pipeline cancellation:

```cpp
pipeline.set_progress_callback([&pipeline](double progress) {
    std::cout << "\rProgress: " << (int)(progress * 100) << "%";

    // User can cancel (e.g., via Ctrl+C handler)
    if (user_requested_cancel) {
        pipeline.cancel();
    }
});
```

The pipeline checks `cancelled_` flag in the main loop:

```cpp
while (!cancelled_) {
    // Process batch
    // ...
}
```

## Stage-Specific Progress Details

### Alignment Stage Sub-Progress

Within the alignment stage, progress is further divided:

1. **Read Parsing** (per batch): ~5% of batch time
2. **GPU Transfer**: ~10% of batch time
3. **GPU Seeding**: ~20% of batch time
4. **GPU Alignment**: ~50% of batch time
5. **GPU Filtering**: ~10% of batch time
6. **Result Transfer**: ~5% of batch time

```cpp
// Example: Update progress during batch processing
void process_batch(size_t batch_num, size_t total_batches) {
    double base_progress = 0.25 + (0.65 * batch_num / total_batches);
    double batch_progress_range = 0.65 / total_batches;

    // Sub-stage 1: GPU transfer
    update_progress(base_progress + 0.05 * batch_progress_range);

    // Sub-stage 2: GPU seeding
    process_seeding();
    update_progress(base_progress + 0.25 * batch_progress_range);

    // Sub-stage 3: GPU alignment
    process_alignment();
    update_progress(base_progress + 0.75 * batch_progress_range);

    // Sub-stage 4: GPU filtering
    process_filtering();
    update_progress(base_progress + 0.95 * batch_progress_range);
}
```

## Best Practices

### For Library Users

1. **Always set a progress callback** for long-running operations
2. **Update UI from callback** on main thread (if using GUI)
3. **Log progress** for batch/server applications
4. **Handle cancellation** gracefully

### For Library Developers

1. **Update progress frequently** (every batch, not every read)
2. **Keep callbacks lightweight** (don't do heavy work in callback)
3. **Provide stage information** when possible
4. **Test with different dataset sizes** to ensure smooth progress

## Example Usage

### Command-Line Application

```cpp
int main(int argc, char* argv[]) {
    Pipeline pipeline(config);

    // Simple progress bar
    pipeline.set_progress_callback([](double p) {
        std::cout << "\r[" << std::string(int(p*50), '=')
                  << std::string(50-int(p*50), ' ')
                  << "] " << int(p*100) << "%" << std::flush;
    });

    pipeline.initialize();
    pipeline.run();
    pipeline.finalize();

    std::cout << "\n";
    return 0;
}
```

### GUI Application (Windows)

```cpp
// In your GUI application
pipeline.set_progress_callback([&progress_bar](double p) {
    // Update progress bar on UI thread
    PostMessage(hwnd_progress, PBM_SETPOS, (WPARAM)(p * 100), 0);
});
```

### Web Service

```cpp
// In your web service
std::atomic<double> current_progress{0.0};

pipeline.set_progress_callback([&current_progress](double p) {
    current_progress.store(p);
});

// Separate thread serves progress updates via REST API
// GET /alignment/progress -> {"progress": 0.65, "stage": "Alignment"}
```

## Performance Considerations

### Callback Frequency

- **Target**: Update every 0.5% - 1% progress change
- **Batch size**: Adjust to balance granularity vs overhead
- **Alignment stage**: Update after each batch completes

### Thread Safety

The progress callback may be called from:
- Main thread (during initialization/finalization)
- Worker threads (during alignment)

Ensure your callback is thread-safe if updating shared state.

### Overhead

Progress monitoring adds minimal overhead:
- **Memory**: ~100 bytes per pipeline instance
- **CPU**: < 0.1% total execution time
- **I/O**: None (unless callback performs I/O)

## Future Enhancements

1. **Hierarchical Progress**: Sub-stage progress within main stages
2. **ETA Calculation**: Estimated time remaining based on throughput
3. **Throughput Metrics**: Reads/second in real-time
4. **Stage Timing**: Per-stage duration statistics
5. **Progress Persistence**: Resume from checkpoint

---

*Last Updated: 2025-11-14*
