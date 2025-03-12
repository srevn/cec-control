#include "command_builder.h"

#include <iostream>
#include <stdexcept>

namespace cec_control {

std::optional<Message> CommandBuilder::buildVolumeCommand(const std::string& action, const std::string& deviceId) {
    uint8_t id = 0;
    if (!parseDeviceId(deviceId, id)) {
        return std::nullopt;
    }
    
    Message cmd;
    cmd.deviceId = id;
    
    if (action == "up") {
        cmd.type = MessageType::CMD_VOLUME_UP;
    } else if (action == "down") {
        cmd.type = MessageType::CMD_VOLUME_DOWN;
    } else if (action == "mute") {
        cmd.type = MessageType::CMD_VOLUME_MUTE;
    } else {
        std::cerr << "Error: Invalid volume action: " << action << std::endl;
        return std::nullopt;
    }
    
    return cmd;
}

std::optional<Message> CommandBuilder::buildPowerCommand(const std::string& action, const std::string& deviceId) {
    uint8_t id = 0;
    if (!parseDeviceId(deviceId, id)) {
        return std::nullopt;
    }
    
    Message cmd;
    cmd.deviceId = id;
    
    if (action == "on") {
        cmd.type = MessageType::CMD_POWER_ON;
    } else if (action == "off") {
        cmd.type = MessageType::CMD_POWER_OFF;
    } else {
        std::cerr << "Error: Invalid power action: " << action << std::endl;
        return std::nullopt;
    }
    
    return cmd;
}

Message CommandBuilder::buildRestartCommand() {
    Message cmd;
    cmd.type = MessageType::CMD_RESTART_ADAPTER;
    cmd.deviceId = 0;  // Device ID is not relevant for restart
    return cmd;
}

Message CommandBuilder::buildSuspendCommand() {
    Message cmd;
    cmd.type = MessageType::CMD_SUSPEND;
    cmd.deviceId = 0;  // Device ID is not relevant for system commands
    return cmd;
}

Message CommandBuilder::buildResumeCommand() {
    Message cmd;
    cmd.type = MessageType::CMD_RESUME;
    cmd.deviceId = 0;  // Device ID is not relevant for system commands
    return cmd;
}

bool CommandBuilder::parseDeviceId(const std::string& deviceId, uint8_t& result) {
    try {
        int val = std::stoi(deviceId);
        if (val < 0 || val > 15) {  // CEC logical addresses are 0-15
            std::cerr << "Error: Device ID must be between 0 and 15" << std::endl;
            return false;
        }
        result = static_cast<uint8_t>(val);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error: Invalid device ID: " << deviceId << std::endl;
        return false;
    }
}

} // namespace cec_control
