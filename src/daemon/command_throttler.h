#pragma once

#include <chrono>
#include <mutex>
#include <atomic>
#include <functional>
#include <thread> 

namespace cec_control {

/**
 * Manages throttling of CEC commands to prevent overwhelming the adapter
 */
class CommandThrottler {
public:
    /**
     * Configuration options for command throttling
     */
    struct Options {
        uint32_t baseIntervalMs;      // Base interval between commands
        uint32_t maxIntervalMs;       // Maximum interval between commands
        uint32_t maxRetryAttempts;    // Maximum retry attempts
        
        Options()
            : baseIntervalMs(100),
              maxIntervalMs(400),
              maxRetryAttempts(3) {}
    };
    
    /**
     * Create a new command throttler
     */
    CommandThrottler(Options options = Options());
    
    /**
     * Execute a command with throttling and retries
     * @param command The command to execute
     * @return true if successful, false otherwise
     */
    bool executeWithThrottle(std::function<bool()> command);
    
    /**
     * Throttle execution based on recent command history
     * @return true if throttling was successful
     */
    bool throttleCommand();
    
    /**
     * Get the number of consecutive failures
     */
    int getConsecutiveFailures() const;
    
    /**
     * Reset consecutive failures count
     */
    void resetConsecutiveFailures();
    
    /**
     * Update command status based on execution result
     * @param success Whether the command was successful
     */
    void updateCommandStatus(bool success);

private:
    Options m_options;
    
    // Command tracking
    struct CommandStatus {
        std::chrono::time_point<std::chrono::steady_clock> lastExecutionTime;
        int consecutiveFailures;
        bool lastCommandSucceeded;
    };
    
    CommandStatus m_commandStatus;
    std::chrono::time_point<std::chrono::steady_clock> m_lastCommandTime;
    mutable std::mutex m_throttleMutex;
    
    // Get adaptive throttle time based on command history
    uint32_t getAdaptiveThrottleTime() const;
    
    // Check if the adapter is busy processing previous commands
    bool isAdapterBusy() const;
};

} // namespace cec_control
