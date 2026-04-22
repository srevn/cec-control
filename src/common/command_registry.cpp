#include "command_registry.h"

#include "key_codes.h"

#include <algorithm>
#include <string>

namespace cec_control {

namespace {

// CEC logical addresses occupy the low 4 bits.
constexpr int kMaxLogicalAddress = 15;

// Source IDs accepted on the wire range over a single byte; the daemon-side
// dispatcher decides which subset map to which physical inputs.
constexpr int kMaxSourceId = 255;

/**
 * Parse a stringified non-negative integer with an inclusive upper bound.
 * Reports a structured error rather than letting std::stoi exceptions escape.
 */
bool parseBoundedUint8(std::string_view in, int maxInclusive,
                       const char* what, uint8_t& out, std::string& err) {
    if (in.empty()) {
        err = std::string("Empty value for ") + what;
        return false;
    }
    try {
        std::size_t consumed = 0;
        const std::string s(in);
        const int v = std::stoi(s, &consumed);
        if (consumed != s.size()) {
            err = std::string("Invalid ") + what + ": '" + s + "'";
            return false;
        }
        if (v < 0 || v > maxInclusive) {
            err = std::string(what) + " must be between 0 and " +
                  std::to_string(maxInclusive);
            return false;
        }
        out = static_cast<uint8_t>(v);
        return true;
    } catch (const std::exception&) {
        err = std::string("Invalid ") + what + ": '" + std::string(in) + "'";
        return false;
    }
}

bool parseDeviceId(std::string_view in, uint8_t& out, std::string& err) {
    return parseBoundedUint8(in, kMaxLogicalAddress, "device ID", out, err);
}

bool requireArity(const std::vector<std::string_view>& args, std::size_t expected,
                  const char* command, const char* syntax, std::string& err) {
    if (args.size() == expected) return true;
    err = std::string(command) + " requires " + std::to_string(expected) +
          " argument(s): " + syntax;
    return false;
}

bool requireNoArgs(const std::vector<std::string_view>& args,
                    const char* command, std::string& err) {
    if (args.empty()) return true;
    err = std::string(command) + " takes no arguments";
    return false;
}

std::optional<Message> parseVolume(const std::vector<std::string_view>& args,
                                    std::string& err) {
    if (!requireArity(args, 2, "volume", "(up|down|mute) DEVICE_ID", err)) {
        return std::nullopt;
    }
    MessageType type;
    if      (args[0] == "up")   type = MessageType::CMD_VOLUME_UP;
    else if (args[0] == "down") type = MessageType::CMD_VOLUME_DOWN;
    else if (args[0] == "mute") type = MessageType::CMD_VOLUME_MUTE;
    else {
        err = "Invalid volume action: '" + std::string(args[0]) +
              "' (expected up|down|mute)";
        return std::nullopt;
    }
    uint8_t id = 0;
    if (!parseDeviceId(args[1], id, err)) return std::nullopt;
    return Message(type, id);
}

std::optional<Message> parsePower(const std::vector<std::string_view>& args,
                                   std::string& err) {
    if (!requireArity(args, 2, "power", "(on|off) DEVICE_ID", err)) {
        return std::nullopt;
    }
    MessageType type;
    if      (args[0] == "on")  type = MessageType::CMD_POWER_ON;
    else if (args[0] == "off") type = MessageType::CMD_POWER_OFF;
    else {
        err = "Invalid power action: '" + std::string(args[0]) +
              "' (expected on|off)";
        return std::nullopt;
    }
    uint8_t id = 0;
    if (!parseDeviceId(args[1], id, err)) return std::nullopt;
    return Message(type, id);
}

std::optional<Message> parseSource(const std::vector<std::string_view>& args,
                                    std::string& err) {
    if (!requireArity(args, 2, "source", "DEVICE_ID SOURCE_ID", err)) {
        return std::nullopt;
    }
    uint8_t id = 0;
    if (!parseDeviceId(args[0], id, err)) return std::nullopt;
    uint8_t source = 0;
    if (!parseBoundedUint8(args[1], kMaxSourceId, "source ID", source, err)) {
        return std::nullopt;
    }
    return Message(MessageType::CMD_CHANGE_SOURCE, id, {source});
}

std::optional<Message> parseKey(const std::vector<std::string_view>& args,
                                 std::string& err) {
    // One-or-two arity: NAME is required, DEVICE_ID defaults to 0 (TV)
    // when omitted. Inline check rather than a new requireArityRange
    // helper — this is the only variable-arity command today.
    if (args.empty() || args.size() > 2) {
        err = "key requires 1 or 2 arguments: NAME [DEVICE_ID]";
        return std::nullopt;
    }
    const KeySpec* spec = findKeyByName(args[0]);
    if (spec == nullptr) {
        err = "Invalid key name: '" + std::string(args[0]) +
              "' (expected " + formatKeyNamesList("|") + ")";
        return std::nullopt;
    }
    uint8_t id = 0;
    if (args.size() == 2 && !parseDeviceId(args[1], id, err)) {
        return std::nullopt;
    }
    return Message(MessageType::CMD_KEY, id, {spec->code});
}

std::optional<Message> parseAutoStandby(const std::vector<std::string_view>& args,
                                         std::string& err) {
    if (!requireArity(args, 1, "auto-standby", "(on|off)", err)) {
        return std::nullopt;
    }
    uint8_t flag;
    if      (args[0] == "on")  flag = 1;
    else if (args[0] == "off") flag = 0;
    else {
        err = "Invalid auto-standby value: '" + std::string(args[0]) +
              "' (expected on|off)";
        return std::nullopt;
    }
    return Message(MessageType::CMD_AUTO_STANDBY, 0, {flag});
}

std::optional<Message> parseRestart(const std::vector<std::string_view>& args,
                                     std::string& err) {
    if (!requireNoArgs(args, "restart", err)) return std::nullopt;
    return Message(MessageType::CMD_RESTART_ADAPTER);
}

std::optional<Message> parseSuspend(const std::vector<std::string_view>& args,
                                     std::string& err) {
    if (!requireNoArgs(args, "suspend", err)) return std::nullopt;
    return Message(MessageType::CMD_SUSPEND);
}

std::optional<Message> parseResume(const std::vector<std::string_view>& args,
                                    std::string& err) {
    if (!requireNoArgs(args, "resume", err)) return std::nullopt;
    return Message(MessageType::CMD_RESUME);
}

} // namespace

// The size of this array is reflected in command_registry.h. If you add an
// entry, bump the std::array<CommandSpec, N> declaration there.
const std::array<CommandSpec, 8> kCommands = {{
    {MessageType::CMD_POWER_ON,
     {MessageType::CMD_POWER_ON, MessageType::CMD_POWER_OFF},
     "power", "(on|off) DEVICE_ID", "Power a device on or off",
     parsePower},
    {MessageType::CMD_VOLUME_UP,
     {MessageType::CMD_VOLUME_UP, MessageType::CMD_VOLUME_DOWN,
      MessageType::CMD_VOLUME_MUTE},
     "volume", "(up|down|mute) DEVICE_ID",
     "Control volume on the audio system",
     parseVolume},
    {MessageType::CMD_CHANGE_SOURCE,
     {MessageType::CMD_CHANGE_SOURCE},
     "source", "DEVICE_ID SOURCE_ID", "Change the active input source",
     parseSource},
    {MessageType::CMD_KEY,
     {MessageType::CMD_KEY},
     "key", "NAME [DEVICE_ID]", "Send a CEC remote-control key press",
     parseKey},
    {MessageType::CMD_AUTO_STANDBY,
     {MessageType::CMD_AUTO_STANDBY},
     "auto-standby", "(on|off)", "Suspend this PC when the TV powers off",
     parseAutoStandby},
    {MessageType::CMD_RESTART_ADAPTER,
     {MessageType::CMD_RESTART_ADAPTER},
     "restart", "", "Restart the CEC adapter",
     parseRestart},
    {MessageType::CMD_SUSPEND,
     {MessageType::CMD_SUSPEND},
     "suspend", "", "Prepare for system sleep (run pre-sleep CEC actions)",
     parseSuspend},
    {MessageType::CMD_RESUME,
     {MessageType::CMD_RESUME},
     "resume", "", "Restore after system wake (run post-wake CEC actions)",
     parseResume},
}};

const CommandSpec* findByName(std::string_view name) noexcept {
    const auto it = std::find_if(kCommands.begin(), kCommands.end(),
        [name](const CommandSpec& c) { return c.name == name; });
    return it == kCommands.end() ? nullptr : &*it;
}

const CommandSpec* findByType(MessageType type) noexcept {
    const auto it = std::find_if(kCommands.begin(), kCommands.end(),
        [type](const CommandSpec& c) {
            return std::find(c.types.begin(), c.types.end(), type)
                   != c.types.end();
        });
    return it == kCommands.end() ? nullptr : &*it;
}

} // namespace cec_control
