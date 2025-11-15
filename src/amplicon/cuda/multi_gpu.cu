#include "winalign/amplicon/cuda/multi_gpu.cuh"
#include <algorithm>
#include <numeric>
#include <chrono>
#include <future>

namespace winalign {
namespace amplicon {
namespace cuda {

/**
 * GPU context switcher implementation
 */
GPUContext::GPUContext(int device_id) {
    cudaGetDevice(&previous_device_);
    cudaSetDevice(device_id);
}

GPUContext::~GPUContext() {
    cudaSetDevice(previous_device_);
}

/**
 * Peer access functions
 */
bool enable_peer_access(int src_device, int dst_device) {
    GPUContext ctx(src_device);

    int can_access = 0;
    cudaDeviceCanAccessPeer(&can_access, src_device, dst_device);

    if (can_access) {
        cudaError_t err = cudaDeviceEnablePeerAccess(dst_device, 0);
        return (err == cudaSuccess || err == cudaErrorPeerAccessAlreadyEnabled);
    }

    return false;
}

bool can_access_peer(int device1, int device2) {
    int can_access = 0;
    cudaDeviceCanAccessPeer(&can_access, device1, device2);
    return can_access != 0;
}

/**
 * Multi-GPU manager implementation
 */
class MultiGPUManager::Impl {
public:
    MultiGPUConfig config_;
    std::vector<GPUDevice> devices_;
    std::vector<std::unique_ptr<MemoryPool>> memory_pools_;
    Stats stats_;

    Impl(const MultiGPUConfig& config)
        : config_(config) {
        stats_ = Stats{{}, {}, {}, 0.0f};
    }

    bool initialize() {
        // Get available devices
        int device_count = 0;
        cudaGetDeviceCount(&device_count);

        if (device_count == 0) {
            return false;
        }

        // If no devices specified, use all available
        if (config_.device_ids.empty()) {
            for (int i = 0; i < device_count; ++i) {
                config_.device_ids.push_back(i);
            }
        }

        // Initialize devices
        for (int device_id : config_.device_ids) {
            if (device_id >= device_count) {
                continue;  // Skip invalid device IDs
            }

            cudaDeviceProp prop;
            cudaGetDeviceProperties(&prop, device_id);

            GPUDevice dev;
            dev.device_id = device_id;
            dev.name = prop.name;
            dev.total_memory = prop.totalGlobalMem;
            dev.compute_capability_major = prop.major;
            dev.compute_capability_minor = prop.minor;
            dev.multiprocessor_count = prop.multiProcessorCount;
            dev.available = true;

            // Get free memory
            {
                GPUContext ctx(device_id);
                size_t free, total;
                cudaMemGetInfo(&free, &total);
                dev.free_memory = free;
            }

            devices_.push_back(dev);

            // Create memory pool for this device
            GPUContext ctx(device_id);
            memory_pools_.push_back(std::make_unique<MemoryPool>());

            // Initialize stats
            stats_.items_processed_per_gpu.push_back(0);
            stats_.gpu_utilization.push_back(0.0f);
            stats_.processing_time_per_gpu.push_back(0.0f);
        }

        // Enable peer access if requested
        if (config_.enable_peer_access && devices_.size() > 1) {
            for (size_t i = 0; i < devices_.size(); ++i) {
                for (size_t j = 0; j < devices_.size(); ++j) {
                    if (i != j) {
                        enable_peer_access(devices_[i].device_id,
                                         devices_[j].device_id);
                    }
                }
            }
        }

        return !devices_.empty();
    }

    std::vector<GPUWorkBatch> distribute_work(
        const void* data,
        size_t total_items,
        size_t item_size
    ) {
        std::vector<GPUWorkBatch> batches;

        if (devices_.empty()) {
            return batches;
        }

        if (config_.auto_balance) {
            // Balance based on GPU memory and compute capability
            std::vector<float> weights;
            float total_weight = 0.0f;

            for (const auto& dev : devices_) {
                float weight = dev.multiprocessor_count *
                              (dev.compute_capability_major * 10 + dev.compute_capability_minor);
                weights.push_back(weight);
                total_weight += weight;
            }

            // Distribute items proportionally
            size_t offset = 0;
            for (size_t i = 0; i < devices_.size(); ++i) {
                size_t count;
                if (i == devices_.size() - 1) {
                    count = total_items - offset;  // Last GPU gets remainder
                } else {
                    count = static_cast<size_t>(
                        (weights[i] / total_weight) * total_items
                    );
                }

                if (count > 0) {
                    GPUWorkBatch batch;
                    batch.device_id = devices_[i].device_id;
                    batch.start_idx = offset;
                    batch.count = count;
                    batch.data_ptr = nullptr;  // Will be allocated later

                    batches.push_back(batch);
                    offset += count;
                }
            }
        } else {
            // Simple round-robin distribution
            size_t items_per_gpu = total_items / devices_.size();
            size_t remainder = total_items % devices_.size();

            size_t offset = 0;
            for (size_t i = 0; i < devices_.size(); ++i) {
                size_t count = items_per_gpu + (i < remainder ? 1 : 0);

                if (count > 0) {
                    GPUWorkBatch batch;
                    batch.device_id = devices_[i].device_id;
                    batch.start_idx = offset;
                    batch.count = count;
                    batch.data_ptr = nullptr;

                    batches.push_back(batch);
                    offset += count;
                }
            }
        }

        return batches;
    }

    void process_batch_on_gpu(
        int gpu_idx,
        const std::vector<ReadCluster>& clusters,
        size_t start_idx,
        size_t count,
        std::vector<AmpliconAlignment>& alignments
    ) {
        GPUContext ctx(devices_[gpu_idx].device_id);
        auto start_time = std::chrono::high_resolution_clock::now();

        // TODO: Implement actual GPU processing
        // This is a placeholder - would call GPU kernels here

        auto end_time = std::chrono::high_resolution_clock::now();
        float duration = std::chrono::duration<float>(end_time - start_time).count();

        stats_.items_processed_per_gpu[gpu_idx] += count;
        stats_.processing_time_per_gpu[gpu_idx] += duration;
    }

    void process_clusters_multi_gpu(
        const std::vector<ReadCluster>& clusters,
        std::vector<AmpliconAlignment>& alignments
    ) {
        if (devices_.size() == 1) {
            // Single GPU - no need for multi-threading
            process_batch_on_gpu(0, clusters, 0, clusters.size(), alignments);
            return;
        }

        // Distribute work across GPUs
        auto batches = distribute_work(nullptr, clusters.size(), sizeof(ReadCluster));

        // Process batches in parallel
        std::vector<std::future<void>> futures;

        for (size_t i = 0; i < batches.size(); ++i) {
            const auto& batch = batches[i];

            futures.push_back(std::async(std::launch::async, [&, i, batch]() {
                process_batch_on_gpu(
                    i, clusters, batch.start_idx, batch.count, alignments
                );
            }));
        }

        // Wait for all to complete
        for (auto& future : futures) {
            future.get();
        }

        // Calculate throughput
        float total_time = *std::max_element(
            stats_.processing_time_per_gpu.begin(),
            stats_.processing_time_per_gpu.end()
        );

        if (total_time > 0.0f) {
            stats_.total_throughput = clusters.size() / total_time;
        }
    }

    void synchronize_all() {
        for (const auto& dev : devices_) {
            GPUContext ctx(dev.device_id);
            cudaDeviceSynchronize();
        }
    }
};

// Public interface

MultiGPUManager::MultiGPUManager(const MultiGPUConfig& config)
    : pimpl_(std::make_unique<Impl>(config)) {}

MultiGPUManager::~MultiGPUManager() {
    pimpl_->synchronize_all();
}

bool MultiGPUManager::initialize() {
    return pimpl_->initialize();
}

std::vector<GPUDevice> MultiGPUManager::get_available_devices() {
    std::vector<GPUDevice> devices;

    int device_count = 0;
    cudaGetDeviceCount(&device_count);

    for (int i = 0; i < device_count; ++i) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, i);

        GPUDevice dev;
        dev.device_id = i;
        dev.name = prop.name;
        dev.total_memory = prop.totalGlobalMem;
        dev.compute_capability_major = prop.major;
        dev.compute_capability_minor = prop.minor;
        dev.multiprocessor_count = prop.multiProcessorCount;
        dev.available = true;

        // Get free memory
        {
            GPUContext ctx(i);
            size_t free, total;
            cudaMemGetInfo(&free, &total);
            dev.free_memory = free;
        }

        devices.push_back(dev);
    }

    return devices;
}

std::vector<GPUWorkBatch> MultiGPUManager::distribute_work(
    const void* data,
    size_t total_items,
    size_t item_size
) {
    return pimpl_->distribute_work(data, total_items, item_size);
}

void MultiGPUManager::process_clusters_multi_gpu(
    const std::vector<ReadCluster>& clusters,
    std::vector<AmpliconAlignment>& alignments
) {
    pimpl_->process_clusters_multi_gpu(clusters, alignments);
}

void MultiGPUManager::call_variants_multi_gpu(
    const std::vector<AmpliconAlignment>& alignments,
    const std::vector<AmpliconTarget>& targets,
    std::vector<AmpliconVariant>& variants
) {
    // Similar to process_clusters_multi_gpu but for variant calling
    // Implementation would follow same pattern
}

void MultiGPUManager::synchronize_all() {
    pimpl_->synchronize_all();
}

MultiGPUManager::Stats MultiGPUManager::get_stats() const {
    return pimpl_->stats_;
}

} // namespace cuda
} // namespace amplicon
} // namespace winalign
