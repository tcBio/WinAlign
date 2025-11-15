#ifndef WINALIGN_AMPLICON_CUDA_MEMORY_POOL_CUH
#define WINALIGN_AMPLICON_CUDA_MEMORY_POOL_CUH

#include <cuda_runtime.h>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace winalign {
namespace amplicon {
namespace cuda {

/**
 * GPU Memory Pool for efficient reuse of allocations
 *
 * Reduces overhead of frequent cudaMalloc/cudaFree calls
 * by maintaining a pool of pre-allocated buffers.
 */
class MemoryPool {
public:
    MemoryPool();
    ~MemoryPool();

    /**
     * Allocate memory from pool
     * Reuses existing allocation if available
     */
    void* allocate(size_t size);

    /**
     * Return memory to pool for reuse
     */
    void deallocate(void* ptr);

    /**
     * Free all pooled memory
     */
    void clear();

    /**
     * Get statistics
     */
    struct Stats {
        uint64_t total_allocations;
        uint64_t pool_hits;
        uint64_t pool_misses;
        uint64_t current_usage;
        uint64_t peak_usage;
        size_t num_blocks;
    };

    Stats get_stats() const;

private:
    struct Block {
        void* ptr;
        size_t size;
        bool in_use;
    };

    std::vector<Block> blocks_;
    std::unordered_map<void*, size_t> ptr_to_block_;

    Stats stats_;

    // Find block of at least requested size
    void* find_free_block(size_t size);

    // Actually allocate new GPU memory
    void* allocate_new(size_t size);
};

/**
 * RAII wrapper for pool-allocated GPU memory
 */
template<typename T>
class PooledArray {
public:
    PooledArray(MemoryPool& pool, size_t count)
        : pool_(pool), ptr_(nullptr), size_(count * sizeof(T)) {
        ptr_ = static_cast<T*>(pool_.allocate(size_));
    }

    ~PooledArray() {
        if (ptr_) {
            pool_.deallocate(ptr_);
        }
    }

    // No copy
    PooledArray(const PooledArray&) = delete;
    PooledArray& operator=(const PooledArray&) = delete;

    // Move only
    PooledArray(PooledArray&& other) noexcept
        : pool_(other.pool_), ptr_(other.ptr_), size_(other.size_) {
        other.ptr_ = nullptr;
        other.size_ = 0;
    }

    T* get() { return ptr_; }
    const T* get() const { return ptr_; }
    size_t size() const { return size_ / sizeof(T); }

private:
    MemoryPool& pool_;
    T* ptr_;
    size_t size_;
};

} // namespace cuda
} // namespace amplicon
} // namespace winalign

#endif // WINALIGN_AMPLICON_CUDA_MEMORY_POOL_CUH
