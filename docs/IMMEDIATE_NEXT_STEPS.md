# Immediate Next Steps - Based on 4% GPU Utilization

**Date**: 2025-11-19
**Critical Issue**: 110 minutes per sample, 4% GPU utilization
**Root Cause**: GPU is starving - I/O or CPU bottleneck, NOT alignment

---

## 🚨 Key Finding

**Batch size is already optimal**: `DEFAULT_BATCH_SIZE = 60,000` (from common.h)
- This is good - not the problem
- **4% GPU usage with 60k batch → I/O or BAM writing is the bottleneck**

---

## 📊 Bottleneck Analysis

### What 4% GPU Means

```
GPU utilization: 4%
GPU idle time: 96%
```

**The GPU is waiting** for:
1. **FASTQ data to be loaded** (I/O bottleneck) - LIKELY
2. **BAM writes to complete** (I/O bottleneck) - VERY LIKELY
3. **CPU preprocessing** (parsing, data prep) - POSSIBLE
4. **Memory transfers** (H2D/D2H) - LESS LIKELY

### Pipeline Flow Analysis

```
[FASTQ Read] → [H2D Transfer] → [GPU Process] → [D2H Transfer] → [BAM Write]
     ↓              ↓                ↓               ↓                ↓
   SLOW?          Fast?           FAST             Fast?            SLOW?
   (disk)       (PCIe)         (4% usage!)       (PCIe)           (disk)
```

**GPU processes in milliseconds**, but waits for I/O in seconds.

---

## 🎯 Immediate Actions (Priority Order)

### Action 1: Add Timing Instrumentation (TODAY - 1 hour)

**Goal**: Measure where time is actually spent

**File**: `src/core/pipeline_multistream.cpp`

**Add this after line 62** (in the `run()` function):

```cpp
// Add timing variables at start of run()
struct StageTiming {
    double load_time = 0;
    double h2d_time = 0;
    double seed_time = 0;
    double align_time = 0;
    double d2h_time = 0;
    double bam_time = 0;
    int batch_count = 0;
};
StageTiming total_timing;

auto timing_start = std::chrono::high_resolution_clock::now();
```

**Add timing around each stage** (insert at appropriate locations):

```cpp
// Around FASTQ loading (line ~100):
auto load_start = std::chrono::high_resolution_clock::now();
if (fastq_worker->get_next_batch(batch)) {
    auto load_end = std::chrono::high_resolution_clock::now();
    double load_ms = std::chrono::duration<double, std::milli>(load_end - load_start).count();
    total_timing.load_time += load_ms;
    // ... rest of load code
}

// Around H2D transfer (line ~345):
auto h2d_start = std::chrono::high_resolution_clock::now();
cudaMemcpyAsync(...);  // existing H2D transfers
auto h2d_end = std::chrono::high_resolution_clock::now();
total_timing.h2d_time += duration(h2d_end - h2d_start);

// Around seeding (add after seeding call):
auto seed_start = std::chrono::high_resolution_clock::now();
// ... seeding kernel launch ...
cudaStreamSynchronize(ctx.stream);  // Wait for seeding
auto seed_end = std::chrono::high_resolution_clock::now();
total_timing.seed_time += duration(seed_end - seed_start);

// Around alignment (add after alignment call):
auto align_start = std::chrono::high_resolution_clock::now();
// ... alignment kernel launch ...
cudaStreamSynchronize(ctx.stream);  // Wait for alignment
auto align_end = std::chrono::high_resolution_clock::now();
total_timing.align_time += duration(align_end - align_start);

// Around D2H transfer (line ~440):
auto d2h_start = std::chrono::high_resolution_clock::now();
cudaMemcpyAsync(...);  // existing D2H transfers
cudaStreamSynchronize(ctx.stream);
auto d2h_end = std::chrono::high_resolution_clock::now();
total_timing.d2h_time += duration(d2h_end - d2h_start);

// Around BAM writing (line ~535):
auto bam_start = std::chrono::high_resolution_clock::now();
std::lock_guard<std::mutex> lock(bam_write_mutex_);
Result<bool> write_result = bam_writer_->write(aln, *current_read_ptr);
auto bam_end = std::chrono::high_resolution_clock::now();
total_timing.bam_time += duration(bam_end - bam_start);
```

**Log summary at end** (before return):

```cpp
// At end of run(), before return:
total_timing.batch_count++;

// Log every 10 batches
if (total_timing.batch_count % 10 == 0) {
    double total_time = total_timing.load_time + total_timing.h2d_time +
                       total_timing.seed_time + total_timing.align_time +
                       total_timing.d2h_time + total_timing.bam_time;

    Logger::instance().info(
        "\n=== Timing Breakdown (avg over " + std::to_string(total_timing.batch_count) + " batches) ===\n" +
        "  FASTQ Load:  " + std::to_string(total_timing.load_time / total_timing.batch_count) + " ms (" +
            std::to_string(100 * total_timing.load_time / total_time) + "%)\n" +
        "  H2D Transfer: " + std::to_string(total_timing.h2d_time / total_timing.batch_count) + " ms (" +
            std::to_string(100 * total_timing.h2d_time / total_time) + "%)\n" +
        "  GPU Seeding:  " + std::to_string(total_timing.seed_time / total_timing.batch_count) + " ms (" +
            std::to_string(100 * total_timing.seed_time / total_time) + "%)\n" +
        "  GPU Alignment: " + std::to_string(total_timing.align_time / total_timing.batch_count) + " ms (" +
            std::to_string(100 * total_timing.align_time / total_time) + "%)\n" +
        "  D2H Transfer: " + std::to_string(total_timing.d2h_time / total_timing.batch_count) + " ms (" +
            std::to_string(100 * total_timing.d2h_time / total_time) + "%)\n" +
        "  BAM Writing:  " + std::to_string(total_timing.bam_time / total_timing.batch_count) + " ms (" +
            std::to_string(100 * total_timing.bam_time / total_time) + "%)\n" +
        "  TOTAL:        " + std::to_string(total_time / total_timing.batch_count) + " ms per batch"
    );
}
```

**Expected Output**:
```
=== Timing Breakdown ===
  FASTQ Load:   500 ms (40%)  ← Likely culprit
  H2D Transfer:  50 ms (4%)
  GPU Seeding:   20 ms (2%)
  GPU Alignment: 30 ms (2%)   ← Only 2% confirms 4% GPU usage
  D2H Transfer:  50 ms (4%)
  BAM Writing:  600 ms (48%)  ← Likely culprit
  TOTAL:       1250 ms per batch
```

**This will tell us exactly where the bottleneck is.**

---

### Action 2: Based on Profiling Results

**If BAM Writing >40% of time** (MOST LIKELY):

**Implement async BAM writing** (2 days):

```cpp
// In pipeline_multistream.h - add member variables:
private:
    // Async BAM writing
    std::queue<std::pair<Alignment, Read>> bam_queue_;
    std::mutex bam_queue_mutex_;
    std::condition_variable bam_queue_cv_;
    std::thread bam_writer_thread_;
    std::atomic<bool> bam_writer_running_{false};

// In pipeline_multistream.cpp - start thread in initialize():
bool MultiStreamScheduler::initialize(...) {
    // ... existing init code ...

    // Start async BAM writer thread
    bam_writer_running_ = true;
    bam_writer_thread_ = std::thread([this]() {
        while (bam_writer_running_ || !bam_queue_.empty()) {
            std::pair<Alignment, Read> item;
            {
                std::unique_lock<std::mutex> lock(bam_queue_mutex_);
                bam_queue_cv_.wait(lock, [this]() {
                    return !bam_queue_.empty() || !bam_writer_running_;
                });

                if (bam_queue_.empty()) break;

                item = bam_queue_.front();
                bam_queue_.pop();
            }

            // Write without blocking main thread
            std::lock_guard<std::mutex> lock(bam_write_mutex_);
            bam_writer_->write(item.first, item.second);
        }
    });

    return true;
}

// In process_context_results() - queue instead of direct write:
// REPLACE:
//   std::lock_guard<std::mutex> lock(bam_write_mutex_);
//   Result<bool> write_result = bam_writer_->write(aln, *current_read_ptr);
//
// WITH:
{
    std::lock_guard<std::mutex> lock(bam_queue_mutex_);
    bam_queue_.push({aln, *current_read_ptr});
    bam_queue_cv_.notify_one();
}
// No waiting - continue immediately!

// In destructor or shutdown - stop thread:
bam_writer_running_ = false;
bam_queue_cv_.notify_all();
if (bam_writer_thread_.joinable()) {
    bam_writer_thread_.join();
}
```

**Expected Impact**: 2-5x speedup if BAM writing is bottleneck

---

**If FASTQ Load >40% of time**:

**Optimize FASTQ reading with mmap** (2-3 days):

```cpp
// In fastq_worker.cpp or fastq_parser.cpp:
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

class MMapFastqParser {
private:
    int fd_;
    char* mapped_;
    size_t file_size_;
    size_t current_pos_;

public:
    MMapFastqParser(const std::string& path) {
        fd_ = open(path.c_str(), O_RDONLY);
        if (fd_ < 0) throw std::runtime_error("Cannot open " + path);

        struct stat sb;
        fstat(fd_, &sb);
        file_size_ = sb.st_size;

        mapped_ = (char*)mmap(NULL, file_size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapped_ == MAP_FAILED) throw std::runtime_error("mmap failed");

        // Advise kernel for sequential read
        madvise(mapped_, file_size_, MADV_SEQUENTIAL);

        current_pos_ = 0;
    }

    ~MMapFastqParser() {
        if (mapped_) munmap(mapped_, file_size_);
        if (fd_ >= 0) close(fd_);
    }

    // Parse directly from memory (much faster than fread)
    bool next_read(Read& read) {
        if (current_pos_ >= file_size_) return false;

        // Parse @ line
        if (mapped_[current_pos_] != '@') return false;
        current_pos_++;

        // Read name (until \n)
        size_t name_start = current_pos_;
        while (current_pos_ < file_size_ && mapped_[current_pos_] != '\n') {
            current_pos_++;
        }
        read.name = std::string(mapped_ + name_start, current_pos_ - name_start);
        current_pos_++;  // Skip \n

        // Read sequence (until \n)
        size_t seq_start = current_pos_;
        while (current_pos_ < file_size_ && mapped_[current_pos_] != '\n') {
            current_pos_++;
        }
        read.sequence = std::string(mapped_ + seq_start, current_pos_ - seq_start);
        current_pos_++;  // Skip \n

        // Skip + line
        while (current_pos_ < file_size_ && mapped_[current_pos_] != '\n') {
            current_pos_++;
        }
        current_pos_++;  // Skip \n

        // Read quality (until \n)
        size_t qual_start = current_pos_;
        while (current_pos_ < file_size_ && mapped_[current_pos_] != '\n') {
            current_pos_++;
        }
        read.quality = std::string(mapped_ + qual_start, current_pos_ - qual_start);
        current_pos_++;  // Skip \n

        return true;
    }
};
```

**Expected Impact**: 2-3x faster FASTQ reading

---

### Action 3: Monitor GPU Utilization

**While implementing fixes, continuously monitor**:

```bash
# Terminal 1: Run winalign
./winalign align --input sample.fastq --output test.bam

# Terminal 2: Monitor GPU
watch -n 1 nvidia-smi

# Look for GPU utilization to increase from 4% → 30-50%+
```

---

## 📊 Expected Results

### Current (Before Fixes)
```
110 minutes per sample
4% GPU utilization
Bottleneck: BAM writing (likely 40-50% of time)
```

### After Async BAM Writing
```
40-60 minutes per sample (2-3x speedup)
8-12% GPU utilization
Bottleneck: FASTQ loading
```

### After FASTQ mmap Optimization
```
20-30 minutes per sample (4-6x speedup)
15-30% GPU utilization
Bottleneck: GPU compute (finally!)
```

### After GPU Optimization (seed chaining)
```
5-10 minutes per sample (10-20x speedup)
40-60% GPU utilization
Balanced pipeline
```

---

## ✅ Summary

### Critical Understanding

**DO NOT optimize GPU code yet!**
- GPU is idle 96% of time
- Optimizing idle GPU won't help
- Fix I/O bottleneck first

### Implementation Order

1. **TODAY**: Add timing instrumentation (1 hour)
2. **Tomorrow**: Rebuild and profile (identify exact bottleneck)
3. **This Week**: Implement fix based on profiling
   - If BAM writing → Async writes
   - If FASTQ loading → mmap
4. **Next Week**: Validate and iterate
5. **Later**: GPU optimizations (only when GPU >30% utilized)

### Expected Timeline

- **Day 1**: Profile and identify bottleneck
- **Day 2-3**: Implement async BAM or mmap FASTQ
- **Day 4**: Test and validate
- **Day 5**: Measure speedup

**Expected result**: 110 min → 20-30 min per sample (4-6x improvement)

---

**Status**: 🔴 URGENT - Add timing instrumentation TODAY
**Next Step**: Copy timing code above into pipeline_multistream.cpp
**Goal**: Find the real bottleneck (BAM or FASTQ)
**Timeline**: 1 hour to add timing, rebuild, and get first results
