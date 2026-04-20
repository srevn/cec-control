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

namespace {

// HDMI source IDs as documented in --help: 2..5 map to HDMI 1..4. The CEC
// physical address byte layout is 0xN000 where N is the HDMI port number.
constexpr uint8_t kFirstHdmiSource = 2;
constexpr uint8_t kLastHdmiSource = 5;

uint16_t hdmiPhysicalAddress(uint8_t source) {
    return static_cast<uint16_t>((source - kFirstHdmiSource + 1) << 12);
}

CEC::cec_user_control_code hdmiNumberKey(uint8_t source) {
    switch (source) {
        case 2: return CEC::CEC_USER_CONTROL_CODE_NUMBER1;
        case 3: return CEC::CEC_USER_CONTROL_CODE_NUMBER2;
        case 4: return CEC::CEC_USER_CONTROL_CODE_NUMBER3;
        case 5: return CEC::CEC_USER_CONTROL_CODE_NUMBER4;
    }
    return CEC::CEC_USER_CONTROL_CODE_UNKNOWN;
}

} // namespace

bool DeviceOperations::setSource(uint8_t logicalAddress, uint8_t source) {
    (void)logicalAddress;
    if (!m_adapter->isConnected()) return false;

    LOG_INFO("Selecting input source ", static_cast<int>(source), " on TV");

    std::lock_guard<std::mutex> lock(m_sourceMutex);

    return m_throttler->executeWithThrottle([this, source]() {
        // Sources 0 and 1 are TV-internal inputs without a CEC physical
        // address; SetStreamPath cannot reach them, so go straight to a
        // function-key keypress.
        auto sendKey = [this](CEC::cec_user_control_code key) -> bool {
            if (!m_adapter->sendKeypress(CEC::CECDEVICE_TV, key, false)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            m_adapter->sendKeypress(CEC::CECDEVICE_TV, CEC::CEC_USER_CONTROL_CODE_UNKNOWN, true);
            return true;
        };

        if (source == 0) {
            return sendKey(CEC::CEC_USER_CONTROL_CODE_SELECT_AV_INPUT_FUNCTION);
        }
        if (source == 1) {
            return sendKey(CEC::CEC_USER_CONTROL_CODE_SELECT_AUDIO_INPUT_FUNCTION);
        }
        if (source < kFirstHdmiSource || source > kLastHdmiSource) {
            LOG_WARNING("Invalid source value: ", source);
            return false;
        }

        // HDMI input: SetStreamPath is the canonical mechanism; fall back to
        // INPUT_SELECT + number keypress sequence if the TV refuses.
        const uint16_t physicalAddress = hdmiPhysicalAddress(source);
        LOG_INFO("Setting stream path to physical address: 0x", std::hex, physicalAddress);

        CEC::ICECAdapter* rawAdapter = m_adapter->getRawAdapter();
        if (rawAdapter && rawAdapter->SetStreamPath(physicalAddress)) {
            return true;
        }

        LOG_INFO("SetStreamPath failed, trying with key presses");
        if (!m_adapter->sendKeypress(CEC::CECDEVICE_TV,
                                     CEC::CEC_USER_CONTROL_CODE_INPUT_SELECT, false)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        const auto numberCode = hdmiNumberKey(source);
        if (!m_adapter->sendKeypress(CEC::CECDEVICE_TV, numberCode, false)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        m_adapter->sendKeypress(CEC::CECDEVICE_TV, CEC::CEC_USER_CONTROL_CODE_UNKNOWN, true);
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
