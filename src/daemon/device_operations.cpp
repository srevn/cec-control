#include "device_operations.h"
#include "../common/logger.h"

namespace cec_control {

DeviceOperations::DeviceOperations(std::shared_ptr<CECAdapter> adapter, std::shared_ptr<CommandThrottler> throttler)
    : m_adapter(adapter), m_throttler(throttler) {
}

bool DeviceOperations::powerOnDevice(uint8_t logicalAddress) {
    if (!m_adapter->isConnected()) return false;
    
    LOG_INFO("Powering on device ", static_cast<int>(logicalAddress));
    
    std::lock_guard<std::mutex> lock(m_powerMutex);
    
    return m_throttler->executeWithThrottle([this, logicalAddress]() {
        CEC::cec_logical_address addr = static_cast<CEC::cec_logical_address>(logicalAddress);
        
        if (!m_adapter->isDeviceActive(addr)) {
            LOG_WARNING("Device ", static_cast<int>(logicalAddress), " is not active");
        }
        
        return m_adapter->powerOnDevice(addr);
    });
}

bool DeviceOperations::powerOffDevice(uint8_t logicalAddress) {
    if (!m_adapter->isConnected()) return false;
    
    LOG_INFO("Powering off device ", static_cast<int>(logicalAddress));
    
    std::lock_guard<std::mutex> lock(m_powerMutex);
    
    return m_throttler->executeWithThrottle([this, logicalAddress]() {
        CEC::cec_logical_address addr = static_cast<CEC::cec_logical_address>(logicalAddress);
        
        if (!m_adapter->isDeviceActive(addr)) {
            LOG_WARNING("Device ", static_cast<int>(logicalAddress), " is not active");
        }
        
        return m_adapter->standbyDevice(addr);
    });
}

bool DeviceOperations::setVolume(uint8_t logicalAddress, bool up) {
    if (!m_adapter->isConnected()) return false;
    
    LOG_INFO("Setting volume ", up ? "up" : "down", " on device ", static_cast<int>(logicalAddress));
    
    std::lock_guard<std::mutex> lock(m_volumeMutex);
    
    return m_throttler->executeWithThrottle([this, up]() {
        // Send volume command and wait for acknowledgment
        if (up) {
            return m_adapter->volumeUp();
        } else {
            return m_adapter->volumeDown();
        }
    });
}

bool DeviceOperations::setMute(uint8_t logicalAddress, bool mute) {
    if (!m_adapter->isConnected()) return false;
    
    LOG_INFO(mute ? "Muting" : "Unmuting", " device ", static_cast<int>(logicalAddress));
    
    std::lock_guard<std::mutex> lock(m_volumeMutex);
    
    return m_throttler->executeWithThrottle([this]() {
        return m_adapter->toggleMute();
    });
}

bool DeviceOperations::setSource(uint8_t logicalAddress, uint8_t source) {
    if (!m_adapter->isConnected()) return false;
    
    LOG_INFO("Selecting input source ", static_cast<int>(source), " on device ", static_cast<int>(logicalAddress));
    
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    
    return m_throttler->executeWithThrottle([this, logicalAddress, source]() {
        // Convert to CEC types
        CEC::cec_logical_address cecAddress = static_cast<CEC::cec_logical_address>(logicalAddress);
        
        // Select the appropriate input based on source value
        // Source values map to CEC user control codes for input selection
        CEC::cec_user_control_code inputCode;
        
        // Map source to appropriate CEC input selection code
        switch (source) {
            case 0: inputCode = CEC::CEC_USER_CONTROL_CODE_SELECT_AV_INPUT_FUNCTION; break;
            case 1: inputCode = CEC::CEC_USER_CONTROL_CODE_SELECT_AUDIO_INPUT_FUNCTION; break;
            case 2: inputCode = CEC::CEC_USER_CONTROL_CODE_F1_BLUE; break; // Can be mapped to HDMI 1
            case 3: inputCode = CEC::CEC_USER_CONTROL_CODE_F2_RED; break;  // Can be mapped to HDMI 2
            case 4: inputCode = CEC::CEC_USER_CONTROL_CODE_F3_GREEN; break; // Can be mapped to HDMI 3
            case 5: inputCode = CEC::CEC_USER_CONTROL_CODE_F4_YELLOW; break; // Can be mapped to HDMI 4
            default: 
                LOG_WARNING("Invalid source value: ", source);
                return false;
        }
        
        // Send the user control code (press)
        if (!m_adapter->sendKeypress(cecAddress, inputCode, false)) {
            LOG_ERROR("Failed to send input selection keypress");
            return false;
        }
        
        // Small delay between press and release
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        // Send the release command
        if (!m_adapter->sendKeypress(cecAddress, inputCode, true)) {
            LOG_ERROR("Failed to send input selection key release");
            return false;
        }
        
        return true;
    });
}

bool DeviceOperations::scanDevices() {
    if (!m_adapter->isConnected()) return false;
    
    std::lock_guard<std::mutex> lock(m_deviceScanMutex);
    
    try {
        // Get active devices
        CEC::cec_logical_addresses addresses = m_adapter->getActiveDevices();
        
        // Count active devices
        int activeCount = 0;
        for (int i = 0; i < 16; i++) {
            if (addresses[i]) {
                activeCount++;
            }
        }
        
        LOG_INFO("Found ", activeCount, " active CEC device(s)");
        
        // Request device power status
        LOG_INFO("Scanning for CEC devices power status...");
        for (int i = 0; i < 15; ++i) {
            CEC::cec_logical_address cecAddress = static_cast<CEC::cec_logical_address>(i);
            CEC::cec_power_status power = m_adapter->getDevicePowerStatus(cecAddress);
            
            const char* status = "unknown";
            switch (power) {
                case CEC::CEC_POWER_STATUS_ON: status = "ON"; break;
                case CEC::CEC_POWER_STATUS_STANDBY: status = "STANDBY"; break;
                case CEC::CEC_POWER_STATUS_IN_TRANSITION_STANDBY_TO_ON: status = "TURNING ON"; break;
                case CEC::CEC_POWER_STATUS_IN_TRANSITION_ON_TO_STANDBY: status = "TURNING OFF"; break;
                default: break;
            }
            
            LOG_INFO("Device ", i, ": Power status = ", status);
        }
        
        // Log active source
        CEC::cec_logical_address active = m_adapter->getActiveSource();
        if (active != CEC::CECDEVICE_UNKNOWN) {
            LOG_INFO("Active source: Device ", static_cast<int>(active));
        } else {
            LOG_INFO("No active source detected");
        }
        
        // Process active devices to get additional details
        for (int i = 0; i < 16; i++) {
            if (addresses[i]) {
                CEC::cec_logical_address addr = static_cast<CEC::cec_logical_address>(i);
                std::string deviceName = m_adapter->getDeviceOSDName(addr);
                bool active = m_adapter->isDeviceActive(addr);
                
                LOG_INFO("Device ", i, ": ", deviceName, " (", active ? "active" : "inactive", ")");
            }
        }
        
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during device scanning: ", e.what());
        return false;
    }
}

} // namespace cec_control
