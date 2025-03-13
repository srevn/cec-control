#include "cec_operation.h"
#include "../common/logger.h"

#include <sstream>

namespace cec_control {

// Initialize static ID counter
std::atomic<uint64_t> CECOperation::s_nextId(1);

CECOperation::CECOperation(const Message& command, Priority priority, uint32_t timeoutMs)
    : m_command(command),
      m_response(MessageType::RESP_ERROR),
      m_creationTime(std::chrono::steady_clock::now()),
      m_priority(priority),
      m_timeoutMs(timeoutMs == 0 ? 5000 : timeoutMs),
      m_id(s_nextId++) {
    
    // Create future from promise
    m_future = m_promise.get_future();
    
    LOG_DEBUG("Created operation #", m_id, ": ", getDescription());
}

CECOperation::~CECOperation() {
    // Set the result if not already done to prevent broken promises
    try {
        if (m_future.valid() && 
            m_future.wait_for(std::chrono::seconds(0)) == std::future_status::timeout) {
            m_promise.set_value();
        }
    } catch (const std::exception& e) {
        LOG_DEBUG("Exception in ~CECOperation for operation #", m_id, ": ", e.what());
    }
}

void CECOperation::setResponse(const Message& response) {
    m_response = response;
}

bool CECOperation::hasTimedOut() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_creationTime).count();
    return elapsed > m_timeoutMs;
}

bool CECOperation::wait(uint32_t timeoutMs) {
    if (timeoutMs == 0) {
        // Use operation's own timeout
        auto waitTime = std::chrono::milliseconds(m_timeoutMs);
        return m_future.wait_for(waitTime) == std::future_status::ready;
    } else {
        // Use provided timeout
        auto waitTime = std::chrono::milliseconds(timeoutMs);
        return m_future.wait_for(waitTime) == std::future_status::ready;
    }
}

void CECOperation::complete(const Message& result) {
    m_response = result;
    
    try {
        m_promise.set_value();
        LOG_DEBUG("Completed operation #", m_id, " with result: ", 
                  (m_response.type == MessageType::RESP_SUCCESS ? "Success" : "Error"));
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to complete operation #", m_id, ": ", e.what());
    }
}

std::string CECOperation::getDescription() const {
    std::stringstream ss;
    
    ss << "Type: ";
    switch (m_command.type) {
        case MessageType::CMD_VOLUME_UP: ss << "Volume Up"; break;
        case MessageType::CMD_VOLUME_DOWN: ss << "Volume Down"; break;
        case MessageType::CMD_VOLUME_MUTE: ss << "Volume Mute"; break;
        case MessageType::CMD_POWER_ON: ss << "Power On"; break;
        case MessageType::CMD_POWER_OFF: ss << "Power Off"; break;
        case MessageType::CMD_CHANGE_SOURCE: ss << "Change Source"; break;
        case MessageType::CMD_RESTART_ADAPTER: ss << "Restart Adapter"; break;
        default: ss << "Unknown (" << static_cast<int>(m_command.type) << ")";
    }
    
    ss << ", DeviceID: " << static_cast<int>(m_command.deviceId);
    
    if (!m_command.data.empty()) {
        ss << ", Data:";
        for (uint8_t b : m_command.data) {
            ss << " " << static_cast<int>(b);
        }
    }
    
    ss << ", Priority: ";
    switch (m_priority) {
        case Priority::HIGH: ss << "HIGH"; break;
        case Priority::NORMAL: ss << "NORMAL"; break;
        case Priority::LOW: ss << "LOW"; break;
    }
    
    return ss.str();
}

bool CECOperation::operator<(const CECOperation& other) const {
    // First compare by priority
    if (m_priority != other.m_priority) {
        return m_priority < other.m_priority;
    }
    
    // Then compare by creation time (older first)
    return m_creationTime > other.m_creationTime;
}

} // namespace cec_control
