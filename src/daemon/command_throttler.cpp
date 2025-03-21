#include "command_throttler.h"
#include "../common/logger.h"

namespace cec_control {

CommandThrottler::CommandThrottler(Options options) 
    : m_options(options), 
      m_commandStatus{std::chrono::steady_clock::now(), 0, true},
      m_lastCommandTime(std::chrono::steady_clock::now() - 
                        std::chrono::milliseconds(m_options.baseIntervalMs)) {
}

bool CommandThrottler::executeWithThrottle(std::function<bool()> command) {
    bool success = false;
    
    for (uint32_t attempt = 0; attempt < m_options.maxRetryAttempts; attempt++) {
        // Throttle command to avoid overwhelming the adapter
        throttleCommand();
        
        // Try to execute the command
        success = command();
        
        // Update command status
        updateCommandStatus(success);
        
        if (success) {
            return true;
        }
        
        // Failed, log and retry after increasing delay
        LOG_WARNING("CEC command failed, retry attempt ", attempt + 1, " of ", m_options.maxRetryAttempts);
        
        // Exponential backoff with special case for first retry
        uint32_t delayMs = (attempt == 0) ? 100 : (100 * (1 << attempt));
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    
    // All retries failed, but we might consider it partial success for volume commands
    LOG_INFO("Command sent but no successful acknowledgment received");
    
    // Don't immediately count this as a complete failure for throttling purposes
    if (m_commandStatus.consecutiveFailures > 0) {
        m_commandStatus.consecutiveFailures--;
    }
    
    return false;
}

bool CommandThrottler::throttleCommand() {
    std::lock_guard<std::mutex> lock(m_throttleMutex);
    
    auto now = std::chrono::steady_clock::now();
    
    // Get adaptive throttle time based on adapter behavior
    uint32_t throttleTime = getAdaptiveThrottleTime();
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastCommandTime).count();
        
    // If not enough time has passed since last command
    if (elapsed < throttleTime) {
        // Calculate how long to wait
        auto sleepTime = throttleTime - elapsed;
        
        LOG_DEBUG("Throttling CEC command for ", sleepTime, "ms (adaptive delay)");
        
        // Sleep for the remaining time to satisfy minimum interval
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
    }
    
    // Update last command time
    m_lastCommandTime = std::chrono::steady_clock::now();
    return true;
}

uint32_t CommandThrottler::getAdaptiveThrottleTime() const {
    // Base throttle time
    if (m_commandStatus.consecutiveFailures == 0) {
        return m_options.baseIntervalMs;
    }
    
    // Exponential backoff based on failure count (limit to 5 failures to prevent overflow)
    uint32_t failureCount = std::min(m_commandStatus.consecutiveFailures, 5u);
    uint32_t additionalDelay = std::min(
        static_cast<uint32_t>(100 * (1 << failureCount)),
        static_cast<uint32_t>(m_options.maxIntervalMs - m_options.baseIntervalMs)
    );
    
    return m_options.baseIntervalMs + additionalDelay;
}

void CommandThrottler::updateCommandStatus(bool success) {
    if (success) {
        m_commandStatus.consecutiveFailures = 0;
        m_commandStatus.lastCommandSucceeded = true;
    } else {
        m_commandStatus.consecutiveFailures++;
        m_commandStatus.lastCommandSucceeded = false;
    }
    
    m_commandStatus.lastExecutionTime = std::chrono::steady_clock::now();
}

int CommandThrottler::getConsecutiveFailures() const {
    return m_commandStatus.consecutiveFailures;
}

void CommandThrottler::resetConsecutiveFailures() {
    m_commandStatus.consecutiveFailures = 0;
}

bool CommandThrottler::isAdapterBusy() const {
    // Consider adapter busy if we had very recent failures
    return (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_commandStatus.lastExecutionTime).count() < 100);
}

} // namespace cec_control
