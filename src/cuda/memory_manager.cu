#include "winalign/cuda/memory_manager.cuh"
#include <cuda_runtime.h>
#include <map>
#include <mutex>
#include <iostream>

namespace winalign {
namespace cuda {

class MemoryManager::Impl {
public:
    Impl(int device_id) : device_id_(device_id), total_memory_(0),
                         free_memory_(0), allocated_memory_(0) {}

    ~Impl() {
        cleanup();
    }

    cudaError_t initialize() {
        // Set device
        cudaError_t err = cudaSetDevice(device_id_);
        if (err != cudaSuccess) {
            return err;
        }

        // Get device properties
        cudaDeviceProp prop;
        err = cudaGetDeviceProperties(&prop, device_id_);
        if (err != cudaSuccess) {
            return err;
        }

        total_memory_ = prop.totalGlobalMem;

        // Get current free memory
        size_t free, total;
        err = cudaMemGetInfo(&free, &total);
        if (err != cudaSuccess) {
            return err;
        }

        free_memory_ = free;

        std::cout << "GPU Memory Manager initialized:\n";
        std::cout << "  Device: " << prop.name << "\n";
        std::cout << "  Total Memory: " << (total_memory_ / (1024 * 1024 * 1024.0)) << " GB\n";
        std::cout << "  Free Memory: " << (free_memory_ / (1024 * 1024 * 1024.0)) << " GB\n";

        return cudaSuccess;
    }

    void cleanup() {
        std::lock_guard<std::mutex> lock(mutex_);

        // Free all active allocations
        for (auto& [ptr, size] : allocations_) {
            cudaFree(ptr);
        }
        allocations_.clear();

        // Free all pooled blocks
        for (auto& [size, ptr] : free_blocks_) {
            cudaFree(ptr);
        }
        free_blocks_.clear();

        // Free all pinned memory
        for (auto& [ptr, size] : pinned_allocations_) {
            cudaFreeHost(ptr);
        }
        pinned_allocations_.clear();

        allocated_memory_ = 0;
    }

    cudaError_t allocate(size_t size, void** ptr) {
        if (!ptr || size == 0) {
            return cudaErrorInvalidValue;
        }

        // Try to reuse a pooled block first
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = free_blocks_.lower_bound(size);
            if (it != free_blocks_.end()) {
                void* dev_ptr = it->second;
                size_t block_size = it->first;
                free_blocks_.erase(it);
                allocations_[dev_ptr] = block_size;
                allocated_memory_ += block_size;
                *ptr = dev_ptr;
                return cudaSuccess;
            }
        }

        // No suitable pooled block; allocate fresh
        void* dev_ptr = nullptr;
        cudaError_t err = cudaMalloc(&dev_ptr, size);
        if (err != cudaSuccess) {
            return err;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        allocations_[dev_ptr] = size;
        allocated_memory_ += size;
        *ptr = dev_ptr;
        return cudaSuccess;
    }

    cudaError_t free(void* ptr) {
        if (!ptr) {
            return cudaSuccess;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = allocations_.find(ptr);
        if (it != allocations_.end()) {
            size_t size = it->second;
            allocated_memory_ -= size;
            allocations_.erase(it);
            // Keep block in a simple pool for reuse
            free_blocks_.emplace(size, ptr);
        }

        return cudaSuccess;
    }

    cudaError_t copy_to_device(void* dst, const void* src, size_t size,
                              cudaStream_t stream) {
        if (stream == 0) {
            return cudaMemcpy(dst, src, size, cudaMemcpyHostToDevice);
        } else {
            return cudaMemcpyAsync(dst, src, size, cudaMemcpyHostToDevice, stream);
        }
    }

    cudaError_t copy_to_host(void* dst, const void* src, size_t size,
                            cudaStream_t stream) {
        if (stream == 0) {
            return cudaMemcpy(dst, src, size, cudaMemcpyDeviceToHost);
        } else {
            return cudaMemcpyAsync(dst, src, size, cudaMemcpyDeviceToHost, stream);
        }
    }

    cudaError_t allocate_pinned(size_t size, void** ptr) {
        if (!ptr) {
            return cudaErrorInvalidValue;
        }

        cudaError_t err = cudaMallocHost(ptr, size);
        if (err != cudaSuccess) {
            return err;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pinned_allocations_[*ptr] = size;

        return cudaSuccess;
    }

    cudaError_t free_pinned(void* ptr) {
        if (!ptr) {
            return cudaSuccess;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pinned_allocations_.find(ptr);
            if (it != pinned_allocations_.end()) {
                pinned_allocations_.erase(it);
            }
        }

        return cudaFreeHost(ptr);
    }

    int get_device_id() const { return device_id_; }
    size_t get_total_memory() const { return total_memory_; }

    size_t get_free_memory() const {
        size_t free, total;
        cudaMemGetInfo(&free, &total);
        return free;
    }

    size_t get_allocated_memory() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return allocated_memory_;
    }

private:
    int device_id_;
    size_t total_memory_;
    size_t free_memory_;
    size_t allocated_memory_;

    std::map<void*, size_t> allocations_;
    std::multimap<size_t, void*> free_blocks_;
    std::map<void*, size_t> pinned_allocations_;
    mutable std::mutex mutex_;
};

// MemoryManager public interface
MemoryManager::MemoryManager(int device_id)
    : pimpl_(std::make_unique<Impl>(device_id)) {}

MemoryManager::~MemoryManager() = default;

cudaError_t MemoryManager::initialize() {
    return pimpl_->initialize();
}

void MemoryManager::cleanup() {
    pimpl_->cleanup();
}

cudaError_t MemoryManager::allocate(size_t size, void** ptr) {
    return pimpl_->allocate(size, ptr);
}

cudaError_t MemoryManager::free(void* ptr) {
    return pimpl_->free(ptr);
}

cudaError_t MemoryManager::copy_to_device(void* dst, const void* src, size_t size,
                                         cudaStream_t stream) {
    return pimpl_->copy_to_device(dst, src, size, stream);
}

cudaError_t MemoryManager::copy_to_host(void* dst, const void* src, size_t size,
                                       cudaStream_t stream) {
    return pimpl_->copy_to_host(dst, src, size, stream);
}

cudaError_t MemoryManager::allocate_pinned(size_t size, void** ptr) {
    return pimpl_->allocate_pinned(size, ptr);
}

cudaError_t MemoryManager::free_pinned(void* ptr) {
    return pimpl_->free_pinned(ptr);
}

int MemoryManager::get_device_id() const {
    return pimpl_->get_device_id();
}

size_t MemoryManager::get_total_memory() const {
    return pimpl_->get_total_memory();
}

size_t MemoryManager::get_free_memory() const {
    return pimpl_->get_free_memory();
}

size_t MemoryManager::get_allocated_memory() const {
    return pimpl_->get_allocated_memory();
}

} // namespace cuda
} // namespace winalign
