#include "command_mapper.h"

#include <iostream>

namespace cec_control {

std::optional<Message> CommandMapper::mapVolumeCommand(const std::string& action,
                                                        const std::string& deviceId) {
    uint8_t id = 0;
    if (!parseDeviceId(deviceId, id)) {
        return std::nullopt;
    }

    MessageType type;
    if (action == "up") {
        type = MessageType::CMD_VOLUME_UP;
    } else if (action == "down") {
        type = MessageType::CMD_VOLUME_DOWN;
    } else if (action == "mute") {
        type = MessageType::CMD_VOLUME_MUTE;
    } else {
        std::cerr << "Error: Invalid volume action: " << action << std::endl;
        return std::nullopt;
    }

    return Message(type, id);
}

std::optional<Message> CommandMapper::mapPowerCommand(const std::string& action,
                                                      const std::string& deviceId) {
    uint8_t id = 0;
    if (!parseDeviceId(deviceId, id)) {
        return std::nullopt;
    }

    MessageType type;
    if (action == "on") {
        type = MessageType::CMD_POWER_ON;
    } else if (action == "off") {
        type = MessageType::CMD_POWER_OFF;
    } else {
        std::cerr << "Error: Invalid power action: " << action << std::endl;
        return std::nullopt;
    }

    return Message(type, id);
}

std::optional<Message> CommandMapper::mapSourceCommand(const std::string& deviceId,
                                                       const std::string& source) {
    uint8_t id = 0;
    if (!parseDeviceId(deviceId, id)) {
        return std::nullopt;
    }

    uint8_t sourceId = 0;
    try {
        int val = std::stoi(source);
        if (val < 0 || val > 255) {
            std::cerr << "Error: Source ID must be between 0 and 255" << std::endl;
            return std::nullopt;
        }
        sourceId = static_cast<uint8_t>(val);
    } catch (const std::exception&) {
        std::cerr << "Error: Invalid source ID: " << source << std::endl;
        return std::nullopt;
    }

    return Message(MessageType::CMD_CHANGE_SOURCE, id, {sourceId});
}

std::optional<Message> CommandMapper::mapAutoStandbyCommand(const std::string& enabled) {
    uint8_t flag;
    if (enabled == "on") {
        flag = 1;
    } else if (enabled == "off") {
        flag = 0;
    } else {
        std::cerr << "Error: Auto-standby must be 'on' or 'off'" << std::endl;
        return std::nullopt;
    }

    return Message(MessageType::CMD_AUTO_STANDBY, 0, {flag});
}

Message CommandMapper::mapRestartCommand() {
    return Message(MessageType::CMD_RESTART_ADAPTER);
}

Message CommandMapper::mapSuspendCommand() {
    return Message(MessageType::CMD_SUSPEND);
}

Message CommandMapper::mapResumeCommand() {
    return Message(MessageType::CMD_RESUME);
}

bool CommandMapper::parseDeviceId(const std::string& deviceId, uint8_t& result) {
    try {
        int val = std::stoi(deviceId);
        if (val < 0 || val > 15) {  // CEC logical addresses are 0-15
            std::cerr << "Error: Device ID must be between 0 and 15" << std::endl;
            return false;
        }
        result = static_cast<uint8_t>(val);
        return true;
    } catch (const std::exception&) {
        std::cerr << "Error: Invalid device ID: " << deviceId << std::endl;
        return false;
    }
}

} // namespace cec_control
