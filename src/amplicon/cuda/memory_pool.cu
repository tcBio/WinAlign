#include "winalign/amplicon/cuda/memory_pool.cuh"
#include <algorithm>
#include <stdexcept>

namespace winalign {
namespace amplicon {
namespace cuda {

MemoryPool::MemoryPool()
    : stats_{0, 0, 0, 0, 0, 0} {
    blocks_.reserve(64);  // Pre-allocate space for block tracking
}

MemoryPool::~MemoryPool() {
    clear();
}

void* MemoryPool::allocate(size_t size) {
    stats_.total_allocations++;

    // Try to find a free block
    void* ptr = find_free_block(size);

    if (ptr) {
        stats_.pool_hits++;
        return ptr;
    }

    // No suitable block found - allocate new
    stats_.pool_misses++;
    return allocate_new(size);
}

void MemoryPool::deallocate(void* ptr) {
    if (!ptr) return;

    auto it = ptr_to_block_.find(ptr);
    if (it == ptr_to_block_.end()) {
        // Not from this pool - this is an error
        return;
    }

    size_t block_idx = it->second;
    if (block_idx >= blocks_.size()) {
        return;
    }

    Block& block = blocks_[block_idx];
    if (!block.in_use) {
        // Double free - this is an error
        return;
    }

    block.in_use = false;
    stats_.current_usage -= block.size;
}

void MemoryPool::clear() {
    // Free all GPU memory
    for (Block& block : blocks_) {
        if (block.ptr) {
            cudaFree(block.ptr);
            block.ptr = nullptr;
        }
    }

    blocks_.clear();
    ptr_to_block_.clear();
    stats_.current_usage = 0;
    stats_.num_blocks = 0;
}

MemoryPool::Stats MemoryPool::get_stats() const {
    return stats_;
}

void* MemoryPool::find_free_block(size_t size) {
    // Find smallest free block that fits
    Block* best_block = nullptr;
    size_t best_size = SIZE_MAX;

    for (Block& block : blocks_) {
        if (!block.in_use && block.size >= size && block.size < best_size) {
            best_block = &block;
            best_size = block.size;

            // Exact match - stop searching
            if (block.size == size) {
                break;
            }
        }
    }

    if (best_block) {
        best_block->in_use = true;
        stats_.current_usage += best_block->size;
        stats_.peak_usage = std::max(stats_.peak_usage, stats_.current_usage);
        return best_block->ptr;
    }

    return nullptr;
}

void* MemoryPool::allocate_new(size_t size) {
    void* ptr = nullptr;
    cudaError_t err = cudaMalloc(&ptr, size);

    if (err != cudaSuccess || !ptr) {
        throw std::runtime_error("GPU memory allocation failed");
    }

    // Add to pool
    Block block;
    block.ptr = ptr;
    block.size = size;
    block.in_use = true;

    size_t block_idx = blocks_.size();
    blocks_.push_back(block);
    ptr_to_block_[ptr] = block_idx;

    stats_.current_usage += size;
    stats_.peak_usage = std::max(stats_.peak_usage, stats_.current_usage);
    stats_.num_blocks = blocks_.size();

    return ptr;
}

} // namespace cuda
} // namespace amplicon
} // namespace winalign
