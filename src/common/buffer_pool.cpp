#include "buffer_pool.h"
#include "logger.h"

namespace cec_control {

BufferPool::BufferPool(size_t bufferSize, size_t initialCapacity)
    : m_bufferSize(bufferSize), m_totalAcquired(0), m_totalReleased(0), m_peakUsage(0) {
    
    // Pre-allocate buffers
    for (size_t i = 0; i < initialCapacity; ++i) {
        auto buffer = std::make_shared<std::vector<uint8_t>>();
        buffer->reserve(m_bufferSize);  // Reserve capacity but don't resize yet
        m_availableBuffers.push(std::move(buffer));
    }
    
    LOG_DEBUG("Created buffer pool with ", initialCapacity, " buffers of size ", bufferSize);
}

std::shared_ptr<std::vector<uint8_t>> BufferPool::acquireBuffer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    m_totalAcquired++;
    size_t currentUsage = m_totalAcquired - m_totalReleased;
    if (currentUsage > m_peakUsage) {
        m_peakUsage = currentUsage;
    }
    
    if (m_availableBuffers.empty()) {
        // Create a new buffer if none are available
        auto newBuffer = std::make_shared<std::vector<uint8_t>>();
        newBuffer->reserve(m_bufferSize);
        return newBuffer;
    }
    
    // Take a buffer from the pool
    auto buffer = m_availableBuffers.front();
    m_availableBuffers.pop();
    
    // Clear it before returning, but keep the capacity
    buffer->clear();
    
    return buffer;
}

void BufferPool::releaseBuffer(std::shared_ptr<std::vector<uint8_t>> buffer) {
    if (!buffer) return;
    
    // Only keep buffers of the right capacity
    if (buffer->capacity() >= m_bufferSize) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_totalReleased++;
        m_availableBuffers.push(std::move(buffer));
    }
}

size_t BufferPool::availableBuffers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_availableBuffers.size();
}

size_t BufferPool::totalAcquired() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalAcquired;
}

size_t BufferPool::totalReleased() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_totalReleased;
}

size_t BufferPool::peakUsage() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_peakUsage;
}

} // namespace cec_control
