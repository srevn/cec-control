#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cec_control {

enum class MessageType : uint8_t {
    // Command messages (client to daemon)
    CMD_VOLUME_UP = 1,
    CMD_VOLUME_DOWN,
    CMD_VOLUME_MUTE,
    CMD_POWER_ON,
    CMD_POWER_OFF,
    CMD_RESTART_ADAPTER,
    CMD_SUSPEND,           // Suspend CEC operations (system sleep)
    CMD_RESUME,            // Resume CEC operations (system wake)
    
    // Response messages (daemon to client)
    RESP_SUCCESS = 100,
    RESP_ERROR,
    RESP_STATUS
};

struct Message {
    MessageType type;
    uint8_t deviceId;  // CEC logical address or broadcast
    std::vector<uint8_t> data;  // Additional parameters

    Message() : type(MessageType::RESP_ERROR), deviceId(0) {}
    
    Message(MessageType type, uint8_t deviceId = 0)
        : type(type), deviceId(deviceId) {}
    
    Message(MessageType type, uint8_t deviceId, const std::vector<uint8_t>& data)
        : type(type), deviceId(deviceId), data(data) {}
    
    // Serialize message to binary format for transmission
    std::vector<uint8_t> serialize() const;
    
    // Deserialize message from binary data
    static Message deserialize(const std::vector<uint8_t>& data);
};

} // namespace cec_control