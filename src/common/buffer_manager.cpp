#include "buffer_manager.h"

namespace cec_control {

BufferPoolManager& BufferPoolManager::getInstance() {
    static BufferPoolManager instance;
    return instance;
}

BufferPool& BufferPoolManager::getPool(size_t bufferSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_pools.find(bufferSize);
    if (it == m_pools.end()) {
        // Create a new pool for this size if it doesn't exist
        size_t initialCapacity = 8;  // Default initial capacity
        auto insertResult = m_pools.emplace(
            bufferSize, 
            std::make_unique<BufferPool>(bufferSize, initialCapacity)
        );
        it = insertResult.first;
    }
    
    return *it->second;
}

} // namespace cec_control
