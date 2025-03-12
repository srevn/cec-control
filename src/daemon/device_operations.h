#pragma once

#include <memory>
#include <mutex>

#include "cec_adapter.h"
#include "command_throttler.h"

namespace cec_control {

/**
 * Handles CEC device operations with proper throttling
 */
class DeviceOperations {
public:
    /**
     * Create a new device operations manager
     * @param adapter The CEC adapter to use
     * @param throttler The command throttler to use
     */
    DeviceOperations(std::shared_ptr<CECAdapter> adapter, std::shared_ptr<CommandThrottler> throttler);
    
    /**
     * Power on a device
     * @param logicalAddress The logical address of the device
     * @return true if successful, false otherwise
     */
    bool powerOnDevice(uint8_t logicalAddress);
    
    /**
     * Power off a device
     * @param logicalAddress The logical address of the device
     * @return true if successful, false otherwise
     */
    bool powerOffDevice(uint8_t logicalAddress);
    
    /**
     * Change the volume
     * @param logicalAddress The logical address of the device
     * @param up true for volume up, false for volume down
     * @return true if successful, false otherwise
     */
    bool setVolume(uint8_t logicalAddress, bool up);
    
    /**
     * Toggle mute on a device
     * @param logicalAddress The logical address of the device
     * @param mute Whether to mute or unmute
     * @return true if successful, false otherwise
     */
    bool setMute(uint8_t logicalAddress, bool mute);
    
    /**
     * Scan for active devices
     * @return true if successful, false otherwise
     */
    bool scanDevices();

private:
    std::shared_ptr<CECAdapter> m_adapter;
    std::shared_ptr<CommandThrottler> m_throttler;
    
    // Mutexes for different operations
    mutable std::mutex m_powerMutex;
    mutable std::mutex m_volumeMutex;
    mutable std::mutex m_deviceScanMutex;
};

} // namespace cec_control
