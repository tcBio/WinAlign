#ifndef WINALIGN_CUDA_MEMORY_MANAGER_CUH
#define WINALIGN_CUDA_MEMORY_MANAGER_CUH

#include <cuda_runtime.h>
#include <cstdint>
#include <memory>

namespace winalign {
namespace cuda {

/**
 * @brief GPU memory manager for WinAlign
 *
 * Manages device memory allocation, transfers, and pooling for optimal performance.
 */
class MemoryManager {
public:
    /**
     * @brief Construct a new Memory Manager
     * @param device_id GPU device ID
     */
    explicit MemoryManager(int device_id = 0);

    /**
     * @brief Destroy the Memory Manager
     */
    ~MemoryManager();

    // Disable copy
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    /**
     * @brief Initialize GPU and allocate initial pools
     * @return cudaError_t CUDA error code
     */
    cudaError_t initialize();

    /**
     * @brief Cleanup and free all GPU memory
     */
    void cleanup();

    /**
     * @brief Allocate device memory
     * @param size Size in bytes
     * @param ptr Output device pointer
     * @return cudaError_t CUDA error code
     */
    cudaError_t allocate(size_t size, void** ptr);

    /**
     * @brief Free device memory
     * @param ptr Device pointer to free
     * @return cudaError_t CUDA error code
     */
    cudaError_t free(void* ptr);

    /**
     * @brief Copy data from host to device
     * @param dst Device destination
     * @param src Host source
     * @param size Size in bytes
     * @param stream CUDA stream (optional)
     * @return cudaError_t CUDA error code
     */
    cudaError_t copy_to_device(void* dst, const void* src, size_t size,
                              cudaStream_t stream = 0);

    /**
     * @brief Copy data from device to host
     * @param dst Host destination
     * @param src Device source
     * @param size Size in bytes
     * @param stream CUDA stream (optional)
     * @return cudaError_t CUDA error code
     */
    cudaError_t copy_to_host(void* dst, const void* src, size_t size,
                            cudaStream_t stream = 0);

    /**
     * @brief Allocate pinned host memory for faster transfers
     * @param size Size in bytes
     * @param ptr Output host pointer
     * @return cudaError_t CUDA error code
     */
    cudaError_t allocate_pinned(size_t size, void** ptr);

    /**
     * @brief Free pinned host memory
     * @param ptr Host pointer to free
     * @return cudaError_t CUDA error code
     */
    cudaError_t free_pinned(void* ptr);

    /**
     * @brief Get device ID
     * @return int Device ID
     */
    int get_device_id() const;

    /**
     * @brief Get total device memory
     * @return size_t Total memory in bytes
     */
    size_t get_total_memory() const;

    /**
     * @brief Get free device memory
     * @return size_t Free memory in bytes
     */
    size_t get_free_memory() const;

    /**
     * @brief Get allocated device memory
     * @return size_t Allocated memory in bytes
     */
    size_t get_allocated_memory() const;

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace cuda
} // namespace winalign

#endif // WINALIGN_CUDA_MEMORY_MANAGER_CUH
