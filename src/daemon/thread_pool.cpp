#include "thread_pool.h"
#include "../common/logger.h"

namespace cec_control {

ThreadPool::ThreadPool(size_t numThreads) 
    : m_stop(false) {
    
    // If numThreads is 0, use hardware concurrency or fallback to 4
    if (numThreads == 0) {
        m_threadCount = std::thread::hardware_concurrency();
        if (m_threadCount == 0) {
            m_threadCount = 4;
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
    // Idempotent: the first call performs the shutdown and logs completion;
    // subsequent calls (e.g. from the destructor after an explicit shutdown)
    // are no-ops. Racing shutdowns from multiple threads is not supported —
    // but covering the explicit-then-dtor path here avoids double-logging.
    if (m_stop.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    m_condition.notify_all();

    for (auto& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    m_workers.clear();

    LOG_INFO("Thread pool shutdown complete");
}

size_t ThreadPool::queueSize() const {
    std::unique_lock<std::mutex> lock(m_queueMutex);
    return m_tasks.size();
}

void ThreadPool::workerThread() {
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
    
    LOG_DEBUG("Worker thread exiting");
}

} // namespace cec_control
