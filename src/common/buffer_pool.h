#pragma once

#include <vector>
#include <mutex>
#include <memory>
#include <queue>

namespace cec_control {

/**
 * A pool of reusable fixed-size buffers to reduce memory allocations
 */
class BufferPool {
public:
    /**
     * Create a buffer pool with the specified buffer size and initial capacity
     * @param bufferSize Size of each buffer in bytes
     * @param initialCapacity Initial number of buffers to pre-allocate
     */
    BufferPool(size_t bufferSize, size_t initialCapacity = 8);
    
    /**
     * Get a buffer from the pool. Creates a new buffer if none are available.
          * @return A shared_ptr to a vector with the appropriate size
     */
    std::shared_ptr<std::vector<uint8_t>> acquireBuffer();
    
    /**
     * Return a buffer to the pool for reuse
     * @param buffer The buffer to return to the pool
     */
    void releaseBuffer(std::shared_ptr<std::vector<uint8_t>> buffer);
    
    /**
     * Get the number of available buffers in the pool
     */
    size_t availableBuffers() const;
    
    /**
     * Get the size of each buffer in bytes
     */
    size_t bufferSize() const { return m_bufferSize; }

    /**
     * Get the total number of buffer acquisitions
     */
    size_t totalAcquired() const;
    
    /**
     * Get the total number of buffer releases
     */
    size_t totalReleased() const;
    
    /**
     * Get the peak usage (max buffers in use simultaneously)
     */
    size_t peakUsage() const;

private:
    size_t m_bufferSize;
    std::queue<std::shared_ptr<std::vector<uint8_t>>> m_availableBuffers;
    mutable std::mutex m_mutex;
    size_t m_totalAcquired = 0;
    size_t m_totalReleased = 0;
    size_t m_peakUsage = 0;
};

} // namespace cec_control
