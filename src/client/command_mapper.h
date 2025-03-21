#pragma once

#include <string>
#include <optional>

#include "../common/messages.h"

namespace cec_control {

/**
 * Utility class to map CEC command messages
 */
class CommandMapper {
public:
    /**
     * Map a volume command (up, down or mute)
     * @param action The action to perform ("up", "down" or "mute")
     * @param deviceId The target device ID (as string, will be parsed)
     * @return The command message, or empty optional on error
     */
    static std::optional<Message> mapVolumeCommand(const std::string& action, const std::string& deviceId);
    
    /**
     * Map a power command (on or off)
     * @param action The action to perform ("on" or "off")
     * @param deviceId The target device ID (as string, will be parsed)
     * @return The command message, or empty optional on error
     */
    static std::optional<Message> mapPowerCommand(const std::string& action, const std::string& deviceId);
    
    /**
     * Map a source change command
     * @param deviceId The target device ID (as string, will be parsed)
     * @param source The source to switch to (as string, will be parsed)
     * @return The command message, or empty optional on error
     */
    static std::optional<Message> mapSourceCommand(const std::string& deviceId, const std::string& source);
    
    /**
     * Map a command to control TV auto-standby feature 
     * (suspend PC when TV powers off)
     * @param enabled "on" or "off"
     * @return The command message, or empty optional on error
     */
    static std::optional<Message> mapAutoStandbyCommand(const std::string& enabled);
    
    /**
     * Map a restart adapter command
     * @return The command message
     */
    static Message mapRestartCommand();
    
    /**
     * Map a system suspend command (for power management)
     * @return The command message
     */
    static Message mapSuspendCommand();
    
    /**
     * Map a system resume command (for power management)
     * @return The command message
     */
    static Message mapResumeCommand();

private:
    /**
     * Parse a device ID string to uint8_t
     * @param deviceId The device ID string
     * @param result Reference to store the parsed ID
     * @return true if successful, false otherwise
     */
    static bool parseDeviceId(const std::string& deviceId, uint8_t& result);
};

} // namespace cec_control
