#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>
#include <memory>
#include <type_traits>
#include <utility>

#include "../common/buffer_pool.h"

namespace cec_control {

/**
 * Thread pool for managing a fixed number of worker threads.
 * Used to handle client connections in a more efficient manner.
 */
class ThreadPool {
public:
    /**
     * Create a thread pool with the specified number of threads
     * @param numThreads Number of worker threads (0 means use hardware concurrency)
     */
    ThreadPool(size_t numThreads = 0);
    
    /**
     * Destructor - waits for all tasks to complete and shuts down threads
     */
    ~ThreadPool();
    
    /**
     * Submit a task to be executed by the thread pool
     * @param task Function to execute
     * @return Future containing the result of the task
     */
    template<class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type>;
    
    /**
     * Gets the number of worker threads in the pool
     */
    size_t size() const { return m_workers.size(); }
    
    /**
     * Gets the number of tasks currently in the queue
     */
    size_t queueSize() const;
    
    /**
     * Initialize pool - creates worker threads
     */
    void start();
    
    /**
     * Shuts down the thread pool - waits for tasks to complete
     */
    void shutdown();
    
private:
    // Worker threads
    std::vector<std::thread> m_workers;
    
    // Task queue
    std::queue<std::function<void()>> m_tasks;
    
    // Synchronization
    mutable std::mutex m_queueMutex;
    std::condition_variable m_condition;
    
    // Shutdown flag
    std::atomic<bool> m_stop;
    
    // Number of threads to create
    size_t m_threadCount;
    
    // Worker thread function
    void workerThread();
    
    // Add task wrapper pool to reduce allocation overhead
    struct TaskWrapper {
        std::function<void()> task;
        void reset() { task = nullptr; }
    };
    
    // Pool of task wrapper objects to reduce allocations
    std::queue<std::shared_ptr<TaskWrapper>> m_taskWrapperPool;
    size_t m_maxPooledTaskWrappers = 100; // Limit pool size
    
    // Get a task wrapper from the pool or create a new one
    std::shared_ptr<TaskWrapper> getTaskWrapper();
    
    // Return a task wrapper to the pool
    void recycleTaskWrapper(std::shared_ptr<TaskWrapper> wrapper);
};

// Template method implementation
template<class F, class... Args>
auto ThreadPool::submit(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
    using return_type = typename std::invoke_result<F, Args...>::type;
    
    // Create a shared_ptr to the packaged_task to store in the queue
    auto task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );
    
    // Get the future before pushing the task
    std::future<return_type> result = task->get_future();
    
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        
        // Don't allow enqueueing after stopping the pool
        if (m_stop) {
            throw std::runtime_error("Cannot enqueue task on stopped ThreadPool");
        }
        
        // Get a task wrapper from pool
        auto wrapper = getTaskWrapper();
        wrapper->task = [task](){ (*task)(); };
        
        // Add task to queue
        m_tasks.emplace([wrapper, this](){
            wrapper->task();
            recycleTaskWrapper(wrapper); // Return to pool after execution
        });
    }
    
    m_condition.notify_one();
    
    return result;
}

} // namespace cec_control
