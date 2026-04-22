#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <optional>
#include <vector>

namespace cec_control {

enum class MessageType : uint8_t {
    // Command messages (client -> daemon)
    CMD_VOLUME_UP = 1,
    CMD_VOLUME_DOWN,
    CMD_VOLUME_MUTE,
    CMD_POWER_ON,
    CMD_POWER_OFF,
    CMD_CHANGE_SOURCE,
    CMD_RESTART_ADAPTER,
    CMD_SUSPEND,
    CMD_RESUME,
    CMD_AUTO_STANDBY,

    // Response messages (daemon -> client)
    RESP_SUCCESS = 100,
    RESP_ERROR,
};

/**
 * Maximum allowed size of one wire-encoded Message. Chosen well above any
 * real payload (current payloads are under 5 bytes); anything larger is
 * treated as malformed and the connection is closed.
 */
constexpr std::size_t MAX_MESSAGE_SIZE = 1024;

struct Message {
    MessageType type;
    uint8_t deviceId;
    std::vector<uint8_t> data;

    // A Message must always have a well-defined type. A default constructor
    // that silently produced RESP_ERROR was ambiguous with real error
    // responses; callers must now construct explicitly.
    Message() = delete;

    Message(MessageType t, uint8_t id = 0) : type(t), deviceId(id) {}

    Message(MessageType t, uint8_t id, std::vector<uint8_t> payload)
        : type(t), deviceId(id), data(std::move(payload)) {}

    /** Serialize to the wire format: [type][deviceId][data...]. */
    std::vector<uint8_t> serialize() const;

    /**
     * Parse a wire-format Message. Returns nullopt if @p data is too short or
     * the type byte is not a known MessageType value.
     */
    static std::optional<Message> deserialize(const uint8_t* data, std::size_t len);
    static std::optional<Message> deserialize(const std::vector<uint8_t>& data);
};

/** Returns true if @p raw is a known MessageType enumerator. */
bool isKnownMessageType(uint8_t raw) noexcept;

/**
 * Response delivery target for a parsed wire command. A sink is
 * invoked exactly once — either synchronously on the handler's thread
 * or through a deferred continuation (typically @c MainThreadWork::post).
 * Single-slot @c std::function matches the one-shot contract at the
 * type level only loosely; the call-at-most-once discipline is
 * enforced by convention at every call site.
 */
using ResponseSink = std::function<void(Message)>;

} // namespace cec_control
