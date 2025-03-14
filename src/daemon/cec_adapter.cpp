#include "cec_adapter.h"

#include <thread>
#include <future>

namespace cec_control {

CECAdapter::CECAdapter(Options options) 
    : m_options(options), m_connected(false) {
    
    // Initialize libcec configuration
    m_config.Clear();
    snprintf(m_config.strDeviceName, sizeof(m_config.strDeviceName), "%s", m_options.deviceName.c_str());
    m_config.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
    m_config.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);
    
    // Set up auto power on and wake AVR
    m_config.bAutoWakeAVR = m_options.autoWakeAVR ? 1 : 0;
    m_config.bAutoPowerOn = m_options.autoPowerOn ? 1 : 0;
    
    // Set up source activation and power off on standby
    m_config.bActivateSource = m_options.activateSource ? 1 : 0;
    m_config.bPowerOffOnStandby = m_options.powerOffOnStandby ? 1 : 0;
    
    // Ensure callbacks structure is allocated properly
    if (!m_config.callbacks) {
        LOG_ERROR("CEC callbacks structure is null, creating a new one");
        m_config.callbacks = new CEC::ICECCallbacks;
        if (!m_config.callbacks) {
            LOG_ERROR("Failed to allocate CEC callbacks structure");
            return;
        }
    }
    
    // Initialize callbacks to null
    m_config.callbacks->logMessage = nullptr;
    m_config.callbacks->commandReceived = nullptr;
    m_config.callbacks->alert = nullptr;
    m_config.callbacks->menuStateChanged = nullptr;
    m_config.callbacks->sourceActivated = nullptr;
    
    // Set up our callbacks
    setupCallbacks();
}

CECAdapter::~CECAdapter() {
    shutdown();
    
    // Clean up callbacks if we allocated them
    if (m_config.callbacks) {
        delete m_config.callbacks;
        m_config.callbacks = nullptr;
    }
}

void CECAdapter::setupCallbacks() {
    if (!m_config.callbacks) {
        LOG_ERROR("Cannot set up callbacks: callback structure is null");
        return;
    }
    
    try {
        // Set up callbacks with safety checks
        m_config.callbacks->logMessage = CECAdapter::cecLogCallback;
        m_config.callbacks->commandReceived = CECAdapter::cecCommandCallback;
        m_config.callbacks->alert = CECAdapter::cecAlertCallback;
        
        // Use a lambda that returns int for menuStateChanged
        m_config.callbacks->menuStateChanged = [](void* param, const CEC::cec_menu_state state) -> int {
            if (param) {
                CECAdapter::cecMenuCallback(param, state);
            }
            return 0;
        };
        
        m_config.callbackParam = this;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during callback setup: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception during callback setup");
    }
}

bool CECAdapter::initialize() {
    LOG_INFO("Initializing CEC adapter");
    
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    
    try {
        // Create libcec interface
        LOG_INFO("Calling CECInitialise...");
        m_adapter = std::unique_ptr<CEC::ICECAdapter>(::CECInitialise(&m_config));
        
        if (!m_adapter) {
            LOG_ERROR("Failed to initialize libCEC - CECInitialise returned null");
            return false;
        }
        
        LOG_INFO("libCEC initialized, version ", m_adapter->VersionToString(m_config.clientVersion));
        
        // Detect adapters
        LOG_INFO("Detecting CEC adapters...");
        CEC::cec_adapter_descriptor devices[10];
        int8_t numDevices = m_adapter->DetectAdapters(devices, 10, nullptr, true);
        
        if (numDevices <= 0) {
            LOG_ERROR("No CEC adapters found");
            m_adapter.reset();
            return false;
        }
        
        LOG_INFO("Found ", static_cast<int>(numDevices), " CEC adapter(s)");
        LOG_INFO("Using adapter: ", devices[0].strComName);
        
        // Open the adapter
        LOG_INFO("Opening CEC adapter...");
        if (!m_adapter->Open(devices[0].strComName)) {
            LOG_ERROR("Failed to open CEC adapter");
            m_adapter.reset();
            return false;
        }
        
        m_connected = true;
        
        // Configure system audio mode
        if (!m_adapter->AudioEnable(m_options.systemAudioMode)) {
            LOG_WARNING("Failed to ", m_options.systemAudioMode ? "enable" : "disable", 
                        " system audio mode");
        } else {
            LOG_INFO("System audio mode ", m_options.systemAudioMode ? "enabled" : "disabled");
        }
        
        LOG_INFO("CEC adapter initialized successfully");
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during CEC initialization: ", e.what());
        m_adapter.reset();
        return false;
    }
    catch (...) {
        LOG_ERROR("Unknown exception during CEC initialization");
        m_adapter.reset();
        return false;
    }
}

void CECAdapter::shutdown() {
    LOG_INFO("Shutting down CEC adapter");
    
    // Set connected to false first to prevent new operations
    m_connected = false;
    
    // Use a timeout to prevent hanging on adapter close
    auto closeWithTimeout = [this]() -> bool {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        
        if (m_adapter) {
            try {
                LOG_INFO("Closing CEC adapter connection");
                m_adapter->Close();
                return true;
            }
            catch (const std::exception& e) {
                LOG_ERROR("Exception during CEC adapter close: ", e.what());
                return false;
            }
            catch (...) {
                LOG_ERROR("Unknown exception during CEC adapter close");
                return false;
            }
        }
        return true;
    };
    
    // Execute close with timeout
    auto closeFuture = std::async(std::launch::async, closeWithTimeout);
    if (closeFuture.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
        LOG_WARNING("CEC adapter close operation timed out");
    }
    
    // Now reset the adapter pointer with lock
    {
        std::lock_guard<std::mutex> lock(m_adapterMutex);
        if (m_adapter) {
            LOG_INFO("Releasing CEC adapter resources");
            m_adapter.reset();
        }
    }
    
    LOG_INFO("CEC adapter shutdown complete");
}

bool CECAdapter::isConnected() const {
    return m_connected;
}

CEC::ICECAdapter* CECAdapter::getRawAdapter() const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    return m_adapter.get();
}

bool CECAdapter::powerOnDevice(CEC::cec_logical_address address) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;
    
    return m_adapter->PowerOnDevices(address);
}

bool CECAdapter::standbyDevice(CEC::cec_logical_address address) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;
    
    return m_adapter->StandbyDevices(address);
}

bool CECAdapter::volumeUp() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;
    
    int result = m_adapter->VolumeUp();
    LOG_DEBUG("Volume up command returned: ", result);
    return isCecSuccess(result);
}

bool CECAdapter::volumeDown() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;
    
    int result = m_adapter->VolumeDown();
    LOG_DEBUG("Volume down command returned: ", result);
    return isCecSuccess(result);
}

bool CECAdapter::toggleMute() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;
    
    int result = m_adapter->AudioToggleMute();
    LOG_DEBUG("Toggle mute command returned: ", result);
    return isCecSuccess(result);
}

bool CECAdapter::sendKeypress(CEC::cec_logical_address address, CEC::cec_user_control_code key, bool release) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;
    
    int result;
    if (release) {
        result = m_adapter->SendKeyRelease(address);
    } else {
        result = m_adapter->SendKeypress(address, key, false);
    }
    
    LOG_DEBUG("Send key", release ? " release" : "press", " command returned: ", result);
    return isCecSuccess(result);
}

uint16_t CECAdapter::getDevicePhysicalAddress(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return 0;
    
    return m_adapter->GetDevicePhysicalAddress(address);
}

bool CECAdapter::isDeviceActive(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;
    
    return m_adapter->IsActiveDevice(address);
}

CEC::cec_power_status CECAdapter::getDevicePowerStatus(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return CEC::CEC_POWER_STATUS_UNKNOWN;
    
    return m_adapter->GetDevicePowerStatus(address);
}

std::string CECAdapter::getDeviceOSDName(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return "";
    
    return m_adapter->GetDeviceOSDName(address);
}

CEC::cec_logical_addresses CECAdapter::getActiveDevices() const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) {
        CEC::cec_logical_addresses empty;
        empty.Clear();
        return empty;
    }
    
    return m_adapter->GetActiveDevices();
}

CEC::cec_logical_address CECAdapter::getActiveSource() const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return CEC::CECDEVICE_UNKNOWN;
    
    return m_adapter->GetActiveSource();
}

bool CECAdapter::hasAdapter() const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    return m_adapter != nullptr;
}

// Callback implementations
void CECAdapter::cecLogCallback(void *cbParam, const CEC::cec_log_message* message) {
    CECAdapter* adapter = static_cast<CECAdapter*>(cbParam);
    if (!adapter || !message) return;
    
    // Map CEC log levels to our log levels
    LogLevel level;
    switch(message->level) {
        case CEC::CEC_LOG_ERROR:   level = LogLevel::ERROR; break;
        case CEC::CEC_LOG_WARNING: level = LogLevel::WARNING; break;
        case CEC::CEC_LOG_NOTICE:  level = LogLevel::INFO; break;
        case CEC::CEC_LOG_TRAFFIC: level = LogLevel::TRAFFIC; break;
        case CEC::CEC_LOG_DEBUG:   level = LogLevel::DEBUG; break;
        default:                   level = LogLevel::INFO; break;
    }
    
    // Log the message
    Logger::getInstance().log(level, "CEC: ", message->message);
}

void CECAdapter::cecCommandCallback(void *cbParam, const CEC::cec_command* command) {
    CECAdapter* adapter = static_cast<CECAdapter*>(cbParam);
    if (!adapter || !command) return;
    
    // Log received commands
    LOG_DEBUG("CEC command received: initiator=", static_cast<int>(command->initiator), 
              ", destination=", static_cast<int>(command->destination),
              ", opcode=", static_cast<int>(command->opcode));
}

void CECAdapter::cecAlertCallback(void *cbParam, const CEC::libcec_alert alert, const CEC::libcec_parameter) {
    CECAdapter* adapter = static_cast<CECAdapter*>(cbParam);
    if (!adapter) return;
    
    switch(alert) {
        case CEC::CEC_ALERT_CONNECTION_LOST:
            LOG_ERROR("CEC connection lost");
            adapter->m_connected = false;
            break;
            
        case CEC::CEC_ALERT_PERMISSION_ERROR:
            LOG_ERROR("CEC permission error");
            break;
            
        case CEC::CEC_ALERT_PORT_BUSY:
            LOG_ERROR("CEC port busy");
            break;
            
        default:
            LOG_DEBUG("CEC alert: ", static_cast<int>(alert));
            break;
    }
}

void CECAdapter::cecMenuCallback(void *cbParam, const CEC::cec_menu_state state) {
    CECAdapter* adapter = static_cast<CECAdapter*>(cbParam);
    if (!adapter) return;
    
    LOG_DEBUG("CEC menu state changed: ", static_cast<int>(state));
}

} // namespace cec_control
