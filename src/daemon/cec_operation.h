#pragma once

#include <future>
#include <memory>
#include <chrono>

#include "../common/messages.h"

namespace cec_control {

/**
 * Represents a single CEC operation to be processed
 */
class CECOperation {
public:
    enum class Priority {
        HIGH,   // Critical operations like restart
        NORMAL, // Standard commands
        LOW     // Background operations
    };

    /**
     * Create a new CEC operation
     *
     * @param command The command message to execute
     * @param priority The operation priority
     * @param timeoutMs The timeout in milliseconds (0 for default)
     */
    CECOperation(const Message& command, Priority priority = Priority::NORMAL, 
                 uint32_t timeoutMs = 5000);
    
    ~CECOperation();
    
    // Get the original command
    const Message& getCommand() const { return m_command; }
    
    // Get/set the response
    const Message& getResponse() const { return m_response; }
    void setResponse(const Message& response);
    
    // Get the timestamp when this operation was created
    std::chrono::time_point<std::chrono::steady_clock> getCreationTime() const { 
        return m_creationTime; 
    }
    
    // Get the priority
    Priority getPriority() const { return m_priority; }
    
    // Has this operation timed out?
    bool hasTimedOut() const;
    
    // Get the operation timeout in milliseconds
    uint32_t getTimeoutMs() const { return m_timeoutMs; }
    
    // Wait for completion with optional timeout
    bool wait(uint32_t timeoutMs = 0);
    
    // Mark operation as complete with result
    void complete(const Message& result);
    
    // Get operation ID
    uint64_t getId() const { return m_id; }
    
    // Get a human-readable description of this operation
    std::string getDescription() const;
    
    // Compare operations (for priority queue)
    bool operator<(const CECOperation& other) const;

private:
    Message m_command;
    Message m_response;
    std::chrono::time_point<std::chrono::steady_clock> m_creationTime;
    Priority m_priority;
    uint32_t m_timeoutMs;
    uint64_t m_id;
    
    std::promise<void> m_promise;
    std::future<void> m_future;
    
    static std::atomic<uint64_t> s_nextId;
};

// Custom comparison for priority queue (inverted for highest priority first)
struct CECOperationComparator {
    bool operator()(const std::shared_ptr<CECOperation>& a, 
                   const std::shared_ptr<CECOperation>& b) const {
        return *b < *a;  // Inverted comparison for max-heap
    }
};

} // namespace cec_control
