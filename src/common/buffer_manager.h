#pragma once

#include <memory>
#include <unordered_map>

#include "buffer_pool.h"

namespace cec_control {

/**
 * Singleton manager for buffer pools of different sizes
 */
class BufferPoolManager {
public:
    static BufferPoolManager& getInstance();
    
    /**
     * Get a buffer pool for the specified size
     * @param bufferSize Size of buffers in the pool
     * @return Reference to the buffer pool
     */
    BufferPool& getPool(size_t bufferSize);
    
private:
    BufferPoolManager() = default;
    ~BufferPoolManager() = default;
    
    // Disable copying and moving
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;
    BufferPoolManager(BufferPoolManager&&) = delete;
    BufferPoolManager& operator=(BufferPoolManager&&) = delete;
    
    std::unordered_map<size_t, std::unique_ptr<BufferPool>> m_pools;
    std::mutex m_mutex;
};

} // namespace cec_control
