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
    (void)logicalAddress;
    if (!m_adapter->isConnected()) return false;
    
    LOG_INFO("Selecting input source ", static_cast<int>(source), " on TV");
    
    std::lock_guard<std::mutex> lock(m_sourceMutex);
    
    return m_throttler->executeWithThrottle([this, source]() {
        // First we'll try using SetStreamPath with physical addresses
        CEC::ICECAdapter* rawAdapter = m_adapter->getRawAdapter();
        if (!rawAdapter) {
            LOG_ERROR("Failed to get raw adapter reference");
            return false;
        }
        
        // Map source to physical address for HDMI ports
        uint16_t physicalAddress = 0;
        switch (source) {
            case 0: physicalAddress = 0x1000; break; // HDMI 1
            case 1: physicalAddress = 0x2000; break; // HDMI 2
            case 2: physicalAddress = 0x1000; break; // HDMI 1
            case 3: physicalAddress = 0x2000; break; // HDMI 2
            case 4: physicalAddress = 0x3000; break; // HDMI 3
            case 5: physicalAddress = 0x4000; break; // HDMI 4
            default:
                LOG_WARNING("Invalid source value: ", source);
                return false;
        }
        
        LOG_INFO("Setting stream path to physical address: 0x", std::hex, physicalAddress);
        
        // Try SetStreamPath - if it fails due to permission issues, fall back to key presses
        if (rawAdapter->SetStreamPath(physicalAddress)) {
            return true;
        }
        
        // If SetStreamPath failed, try with key presses to the TV
        LOG_INFO("SetStreamPath failed, trying with key presses");
        
        bool result = false;
        
        // For general inputs (0-1), use the specific function keys
        if (source == 0) {
            // General AV input
            result = m_adapter->sendKeypress(CEC::CECDEVICE_TV, 
                CEC::CEC_USER_CONTROL_CODE_SELECT_AV_INPUT_FUNCTION, false);
        } else if (source == 1) {
            // Audio input
            result = m_adapter->sendKeypress(CEC::CECDEVICE_TV, 
                CEC::CEC_USER_CONTROL_CODE_SELECT_AUDIO_INPUT_FUNCTION, false);
        } else {
            // For HDMI 1-4, first send INPUT_SELECT, then the appropriate number
            result = m_adapter->sendKeypress(CEC::CECDEVICE_TV, 
                CEC::CEC_USER_CONTROL_CODE_INPUT_SELECT, false);
                
            if (result) {
                // Wait a short time between keypresses
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                
                // Choose the right number key for the HDMI input
                // HDMI 1 = source 2, send NUMBER1
                // HDMI 2 = source 3, send NUMBER2
                // HDMI 3 = source 4, send NUMBER3
                // HDMI 4 = source 5, send NUMBER4
                CEC::cec_user_control_code numberCode;
                switch (source) {
                    case 2: numberCode = CEC::CEC_USER_CONTROL_CODE_NUMBER1; break;
                    case 3: numberCode = CEC::CEC_USER_CONTROL_CODE_NUMBER2; break;
                    case 4: numberCode = CEC::CEC_USER_CONTROL_CODE_NUMBER3; break;
                    case 5: numberCode = CEC::CEC_USER_CONTROL_CODE_NUMBER4; break;
                    default: return false;
                }
                
                // Send the number key
                result = m_adapter->sendKeypress(CEC::CECDEVICE_TV, numberCode, false);
            }
        }
        
        // Send key release if initial press was successful
        if (result) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            m_adapter->sendKeypress(CEC::CECDEVICE_TV, CEC::CEC_USER_CONTROL_CODE_UNKNOWN, true);
        }
        
        return result;
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
