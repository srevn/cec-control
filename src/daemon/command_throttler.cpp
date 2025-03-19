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
    bool commandSent = false;  // Track if at least one attempt was made
    
    for (uint32_t attempt = 0; attempt < m_options.maxRetryAttempts; attempt++) {
        // Throttle command to avoid overwhelming the adapter
        throttleCommand();
        
        // Try to execute the command
        success = command();
        commandSent = true;
        
        // Update command status
        updateCommandStatus(success);
        
        if (success) {
            return true; // Success!
        }
        
        // Failed, log and retry after increasing delay
        LOG_WARNING("CEC command failed, retry attempt ", attempt + 1, " of ", m_options.maxRetryAttempts);
        
        // For volume commands, we might want to retry less aggressively since they often work
        // even when reporting failure due to no acknowledgment
        if (attempt == 0) {
            // First retry quickly
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } else {
            // Subsequent retries with exponential backoff
            std::this_thread::sleep_for(std::chrono::milliseconds(
                50 * (1 << attempt)));  // 50ms, 100ms, 200ms
        }
    }
    
    // If we made at least one attempt, consider it partial success
    // This helps with volume commands which work but don't get acknowledgments
    if (commandSent) {
        LOG_INFO("Command sent but no successful acknowledgment received");
        // Don't immediately count this as a failure for throttling purposes
        if (m_commandStatus.consecutiveFailures > 0) {
            m_commandStatus.consecutiveFailures--;
        }
    }
    
    // All retries failed
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
    
    // Wait until adapter is no longer busy
    if (isAdapterBusy()) {
        LOG_DEBUG("Waiting for CEC adapter to be ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    // Update last command time
    m_lastCommandTime = std::chrono::steady_clock::now();
    return true;
}

uint32_t CommandThrottler::getAdaptiveThrottleTime() const {
    // Base throttle time
    uint32_t throttleTime = m_options.baseIntervalMs;
    
    // Increase throttle time based on consecutive failures
    if (m_commandStatus.consecutiveFailures > 0) {
        // Exponential backoff based on failure count
        uint32_t additionalDelay = std::min(
            static_cast<uint32_t>(50 * (1 << std::min(m_commandStatus.consecutiveFailures, 5))),
            static_cast<uint32_t>(m_options.maxIntervalMs - m_options.baseIntervalMs)
        );
        
        throttleTime += additionalDelay;
    }
    
    return throttleTime;
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
    bool recentFailure = (m_commandStatus.consecutiveFailures > 0) && 
        (std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - m_commandStatus.lastExecutionTime).count() < 50);
    
    return recentFailure;
}

} // namespace cec_control
