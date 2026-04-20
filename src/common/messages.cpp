#include "messages.h"

namespace cec_control {

bool isKnownMessageType(uint8_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::CMD_VOLUME_UP:
        case MessageType::CMD_VOLUME_DOWN:
        case MessageType::CMD_VOLUME_MUTE:
        case MessageType::CMD_POWER_ON:
        case MessageType::CMD_POWER_OFF:
        case MessageType::CMD_CHANGE_SOURCE:
        case MessageType::CMD_RESTART_ADAPTER:
        case MessageType::CMD_SUSPEND:
        case MessageType::CMD_RESUME:
        case MessageType::CMD_AUTO_STANDBY:
        case MessageType::RESP_SUCCESS:
        case MessageType::RESP_ERROR:
        case MessageType::RESP_STATUS:
            return true;
    }
    return false;
}

std::vector<uint8_t> Message::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(2 + data.size());
    out.push_back(static_cast<uint8_t>(type));
    out.push_back(deviceId);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::optional<Message> Message::deserialize(const uint8_t* data, std::size_t len) {
    if (len < 2 || len > MAX_MESSAGE_SIZE) {
        return std::nullopt;
    }
    if (!isKnownMessageType(data[0])) {
        return std::nullopt;
    }

    return Message(
        static_cast<MessageType>(data[0]),
        data[1],
        std::vector<uint8_t>(data + 2, data + len));
}

std::optional<Message> Message::deserialize(const std::vector<uint8_t>& data) {
    return deserialize(data.data(), data.size());
}

} // namespace cec_control
