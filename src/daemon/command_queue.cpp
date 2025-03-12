#include "command_queue.h"
#include "../common/logger.h"

#include <algorithm>

namespace cec_control {

CommandQueue::CommandQueue() 
    : m_running(false), m_processedCount(0) {
}

CommandQueue::~CommandQueue() {
    stop();
}

bool CommandQueue::start() {
    if (m_running) {
        LOG_WARNING("Command queue already started");
        return true;
    }
    
    LOG_INFO("Starting command queue worker thread");
    m_running = true;
    m_workerThread = std::thread(&CommandQueue::workerLoop, this);
    
    return true;
}

void CommandQueue::stop() {
    if (!m_running) {
        return;
    }
    
    LOG_INFO("Stopping command queue worker thread");
    m_running = false;
    
    // Notify the worker to check the running flag
    m_queueCondition.notify_all();
    
    // Wait for the thread to exit with timeout
    if (m_workerThread.joinable()) {
        // Use async with future to implement timeout for thread join
        auto joinFuture = std::async(std::launch::async, [this]() {
            if (m_workerThread.joinable()) {
                m_workerThread.join();
                return true;
            }
            return false;
        });
        
        // Wait for thread to join with 3 second timeout
        if (joinFuture.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
            LOG_WARNING("Command queue worker thread did not exit cleanly within timeout");
        } else {
            LOG_INFO("Command queue worker thread joined successfully");
        }
    }
    
    // Cancel all pending operations
    cancelAll();
    
    LOG_INFO("Command queue worker thread stopped");
}

void CommandQueue::setOperationHandler(OperationHandler handler) {
    m_handler = handler;
}

std::shared_ptr<CECOperation> CommandQueue::enqueue(
    const Message& command, 
    CECOperation::Priority priority,
    uint32_t timeoutMs) {
    
    if (!m_running || !m_handler) {
        LOG_ERROR("Cannot enqueue operation: queue not running or no handler set");
        auto operation = std::make_shared<CECOperation>(command, priority, timeoutMs);
        operation->complete(Message(MessageType::RESP_ERROR));
        return operation;
    }
    
    auto operation = std::make_shared<CECOperation>(command, priority, timeoutMs);
    
    // Add directly to the queue without trying to merge
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push(operation);
        m_activeOperations[operation->getId()] = operation;
        
        // Log the enqueued operation for debugging
        LOG_DEBUG("Enqueued operation: ", operation->getDescription(), 
                  " (queue size: ", m_queue.size(), ")");
    }
    
    // Notify worker thread of new operation
    m_queueCondition.notify_one();
    
    return operation;
}

Message CommandQueue::executeSync(const Message& command, uint32_t timeoutMs) {
    // Special handling for restart command - use high priority
    CECOperation::Priority priority = (command.type == MessageType::CMD_RESTART_ADAPTER) 
        ? CECOperation::Priority::HIGH 
        : CECOperation::Priority::NORMAL;
    
    auto operation = enqueue(command, priority, timeoutMs);
    
    // Wait for operation to complete
    if (operation->wait(timeoutMs)) {
        return operation->getResponse();
    } else {
        LOG_WARNING("Operation timed out: ", operation->getDescription());
        return Message(MessageType::RESP_ERROR);
    }
}

size_t CommandQueue::getPendingCount() const {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_queue.size();
}

void CommandQueue::cancelAll() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    
    // Create a temporary queue to swap with the current queue
    std::priority_queue<std::shared_ptr<CECOperation>, 
                       std::vector<std::shared_ptr<CECOperation>>,
                       CECOperationComparator> emptyQueue;
    std::swap(m_queue, emptyQueue);
    
    // Complete all operations with error
    while (!emptyQueue.empty()) {
        auto operation = emptyQueue.top();
        emptyQueue.pop();
        
        operation->complete(Message(MessageType::RESP_ERROR));
    }
    
    // Complete active operations
    for (auto& pair : m_activeOperations) {
        pair.second->complete(Message(MessageType::RESP_ERROR));
    }
    
    m_activeOperations.clear();
}

void CommandQueue::workerLoop() {
    LOG_INFO("Command queue worker thread started");
    
    while (m_running) {
        std::shared_ptr<CECOperation> operation;
        
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            
            // Clean up old operations
            cleanupOperations();
            
            // Wait for a command or until we're told to stop
            m_queueCondition.wait(lock, [this] { 
                return !m_running || !m_queue.empty(); 
            });
            
            // Check if we should exit
            if (!m_running) {
                break;
            }
            
            // Get the next operation
            if (!m_queue.empty()) {
                operation = m_queue.top();
                m_queue.pop();
            }
        }
        
        // Process the operation outside the lock
        if (operation) {
            processOperation(operation);
        }
    }
}

void CommandQueue::processOperation(std::shared_ptr<CECOperation> operation) {
    if (!operation || !m_handler) {
        return;
    }
    
    try {
        // Check if operation has timed out
        if (operation->hasTimedOut()) {
            LOG_WARNING("Operation timed out before processing: ", operation->getDescription());
            operation->complete(Message(MessageType::RESP_ERROR));
            
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_activeOperations.erase(operation->getId());
            return;
        }
        
        // Log the operation being processed
        const Message& cmd = operation->getCommand();
        LOG_DEBUG("Processing operation: type=", static_cast<int>(cmd.type), 
                  ", device=", static_cast<int>(cmd.deviceId), 
                  ", priority=", static_cast<int>(operation->getPriority()));
        
        // Process with reliable completion tracking
        Message result;
        int maxAttempts = 2;  // Allow one retry if needed
        
        for (int attempt = 0; attempt < maxAttempts; attempt++) {
            result = m_handler(operation->getCommand());
            
            // If successful or explicitly failed, we can stop
            if (result.type == MessageType::RESP_SUCCESS || 
                result.type == MessageType::RESP_ERROR) {
                break;
            }
            
            // Otherwise, pause briefly and try again
            if (attempt < maxAttempts - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                LOG_INFO("Retrying operation due to indeterminate result");
            }
        }
        
        // Log the result
        LOG_DEBUG("Operation completed: ", operation->getDescription(), 
                  " result=", static_cast<int>(result.type));
        
        // Update the operation with the result
        operation->complete(result);
        m_processedCount++;
        
        // Remove from active operations
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_activeOperations.erase(operation->getId());
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception processing operation: ", e.what());
        operation->complete(Message(MessageType::RESP_ERROR));
        
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_activeOperations.erase(operation->getId());
    }
}

void CommandQueue::cleanupOperations() {
    // Caller must hold the queue mutex
    
    // Clean up timed out operations
    auto it = m_activeOperations.begin();
    while (it != m_activeOperations.end()) {
        if (it->second->hasTimedOut()) {
            LOG_WARNING("Operation timed out: ", it->second->getDescription());
            it->second->complete(Message(MessageType::RESP_ERROR));
            it = m_activeOperations.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace cec_control
