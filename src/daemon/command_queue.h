#pragma once

#include <queue>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <unordered_map>

#include "cec_operation.h"
#include "../common/logger.h"

namespace cec_control {

/**
 * A queue for processing CEC operations with a dedicated worker thread
 */
class CommandQueue {
public:
    using OperationHandler = std::function<Message(const Message&)>;
    
    CommandQueue();
    ~CommandQueue();
    
    // Start the worker thread
    bool start();
    
    // Stop the worker thread
    void stop();
    
    // Set the handler for processing operations
    void setOperationHandler(OperationHandler handler);
    
    // Enqueue a new operation and get a shared_ptr to track it
    std::shared_ptr<CECOperation> enqueue(const Message& command, 
                                         CECOperation::Priority priority = CECOperation::Priority::NORMAL,
                                         uint32_t timeoutMs = 5000);
    
    // Execute a command synchronously with timeout
    Message executeSync(const Message& command, uint32_t timeoutMs = 5000);
    
    // Is the queue running?
    bool isRunning() const { return m_running; }
    
    // Get the number of pending operations
    size_t getPendingCount() const;
    
    // Get the number of operations processed
    uint64_t getProcessedCount() const { return m_processedCount; }
    
    // Cancel all pending operations
    void cancelAll();

private:
    // Priority queue for operations
    std::priority_queue<std::shared_ptr<CECOperation>, 
                       std::vector<std::shared_ptr<CECOperation>>,
                       CECOperationComparator> m_queue;
                       
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCondition;
    std::atomic<bool> m_running;
    std::thread m_workerThread;
    OperationHandler m_handler;
    std::atomic<uint64_t> m_processedCount;
    
    // Track active operations
    std::unordered_map<uint64_t, std::shared_ptr<CECOperation>> m_activeOperations;
    
    // Worker thread function
    void workerLoop();
    
    // Process a single operation
    void processOperation(std::shared_ptr<CECOperation> operation);
    
    // Clean up completed and timed out operations
    void cleanupOperations();
};

} // namespace cec_control
