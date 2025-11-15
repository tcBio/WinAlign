#ifndef WINALIGN_AMPLICON_CUDA_MULTI_GPU_CUH
#define WINALIGN_AMPLICON_CUDA_MULTI_GPU_CUH

#include "../amplicon_types.h"
#include "memory_pool.cuh"
#include <cuda_runtime.h>
#include <vector>
#include <memory>
#include <thread>

namespace winalign {
namespace amplicon {
namespace cuda {

/**
 * GPU device information
 */
struct GPUDevice {
    int device_id;
    std::string name;
    size_t total_memory;
    size_t free_memory;
    int compute_capability_major;
    int compute_capability_minor;
    int multiprocessor_count;
    bool available;
};

/**
 * Multi-GPU configuration
 */
struct MultiGPUConfig {
    std::vector<int> device_ids;        // Which GPUs to use
    bool auto_balance;                   // Automatically balance load
    size_t batch_size_per_gpu;          // Batch size for each GPU
    bool enable_peer_access;             // Enable GPU-GPU communication

    MultiGPUConfig()
        : auto_balance(true), batch_size_per_gpu(10000),
          enable_peer_access(false) {}
};

/**
 * Work distribution for multi-GPU processing
 */
struct GPUWorkBatch {
    int device_id;
    size_t start_idx;
    size_t count;
    void* data_ptr;
};

/**
 * Multi-GPU manager for amplicon processing
 *
 * Distributes work across multiple GPUs for maximum throughput.
 */
class MultiGPUManager {
public:
    MultiGPUManager(const MultiGPUConfig& config = MultiGPUConfig());
    ~MultiGPUManager();

    /**
     * Initialize all GPUs
     */
    bool initialize();

    /**
     * Get available GPU devices
     */
    static std::vector<GPUDevice> get_available_devices();

    /**
     * Distribute data across GPUs
     */
    std::vector<GPUWorkBatch> distribute_work(
        const void* data,
        size_t total_items,
        size_t item_size
    );

    /**
     * Process read clusters on multiple GPUs
     */
    void process_clusters_multi_gpu(
        const std::vector<ReadCluster>& clusters,
        std::vector<AmpliconAlignment>& alignments
    );

    /**
     * Call variants on multiple GPUs
     */
    void call_variants_multi_gpu(
        const std::vector<AmpliconAlignment>& alignments,
        const std::vector<AmpliconTarget>& targets,
        std::vector<AmpliconVariant>& variants
    );

    /**
     * Synchronize all GPUs
     */
    void synchronize_all();

    /**
     * Get statistics
     */
    struct Stats {
        std::vector<uint64_t> items_processed_per_gpu;
        std::vector<float> gpu_utilization;
        std::vector<float> processing_time_per_gpu;
        float total_throughput;  // items/sec
    };

    Stats get_stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

/**
 * RAII GPU context switcher
 */
class GPUContext {
public:
    explicit GPUContext(int device_id);
    ~GPUContext();

    GPUContext(const GPUContext&) = delete;
    GPUContext& operator=(const GPUContext&) = delete;

private:
    int previous_device_;
};

/**
 * Enable peer-to-peer access between GPUs
 */
bool enable_peer_access(int src_device, int dst_device);

/**
 * Check if two GPUs can communicate directly
 */
bool can_access_peer(int device1, int device2);

} // namespace cuda
} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_CUDA_MULTI_GPU_CUH
