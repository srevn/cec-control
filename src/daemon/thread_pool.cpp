#include "thread_pool.h"
#include "../common/logger.h"

namespace cec_control {

ThreadPool::ThreadPool(size_t numThreads) 
    : m_stop(false) {
    
    // If numThreads is 0, use hardware concurrency or fallback to 4
    if (numThreads == 0) {
        m_threadCount = std::thread::hardware_concurrency();
        if (m_threadCount == 0) {
            m_threadCount = 4; // Default to 4 threads if hardware_concurrency is not supported
        }
    } else {
        m_threadCount = numThreads;
    }
    
    LOG_INFO("Creating thread pool with ", m_threadCount, " worker threads");
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::start() {
    // Only start if the pool is not already running
    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (!m_stop && m_workers.empty()) {
        // Create worker threads
        m_workers.reserve(m_threadCount);
        for (size_t i = 0; i < m_threadCount; ++i) {
            m_workers.emplace_back(&ThreadPool::workerThread, this);
            LOG_DEBUG("Thread pool: created worker thread #", i + 1);
        }
        LOG_INFO("Thread pool started with ", m_threadCount, " worker threads");
    }
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        
        // Set stop flag and notify all threads
        m_stop = true;
    }
    
    // Notify all threads to wake up
    m_condition.notify_all();
    
    // Wait for all threads to finish
    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            try {
                worker.join();
            } catch (const std::exception& e) {
                LOG_ERROR("Exception when joining worker thread: ", e.what());
            }
        }
    }
    
    // Clear worker threads
    m_workers.clear();
    LOG_INFO("Thread pool shutdown complete");
}

size_t ThreadPool::queueSize() const {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    return m_tasks.size();
}

void ThreadPool::workerThread() {
    try {
        while (true) {
            std::function<void()> task;
            
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                
                // Wait for task or stop signal
                m_condition.wait(lock, [this] {
                    return m_stop || !m_tasks.empty();
                });
                
                // If stopping and no tasks left, exit
                if (m_stop && m_tasks.empty()) {
                    break;
                }
                
                // Get task from queue
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            
            // Execute task
            try {
                task();
            }
            catch (const std::exception& e) {
                LOG_ERROR("Exception in worker thread task: ", e.what());
            }
            catch (...) {
                LOG_ERROR("Unknown exception in worker thread task");
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception in worker thread: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception in worker thread");
    }
    
    LOG_DEBUG("Worker thread exiting");
}

std::shared_ptr<ThreadPool::TaskWrapper> ThreadPool::getTaskWrapper() {
    // Must be called with m_queueMutex locked
    if (!m_taskWrapperPool.empty()) {
        auto wrapper = m_taskWrapperPool.front();
        m_taskWrapperPool.pop();
        return wrapper;
    }
    
    return std::make_shared<TaskWrapper>();
}

void ThreadPool::recycleTaskWrapper(std::shared_ptr<TaskWrapper> wrapper) {
    if (!wrapper) return;
    
    wrapper->reset();
    
    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (m_taskWrapperPool.size() < m_maxPooledTaskWrappers) {
        m_taskWrapperPool.push(std::move(wrapper));
    }
    // If pool is full, let the wrapper be destroyed
}

} // namespace cec_control
