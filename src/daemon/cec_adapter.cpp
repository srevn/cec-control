#include "cec_adapter.h"
#include "../common/logger.h"
#include "../common/config_manager.h"

#include <thread>
#include <future>
#include <unistd.h>
#include <sys/types.h>

namespace cec_control {

CECAdapter::CECAdapter(Options options)
    : m_options(options), m_connected(false) {

    // Initialize libcec configuration
    m_config.Clear();
    m_config.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
    m_config.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);

    // Populate config from options
    populateConfigFromOptions(m_options);

    // Ensure callbacks structure is allocated properly
    if (!m_config.callbacks) {
        LOG_INFO("Allocating CEC callbacks structure");
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
    m_config.callbacks->sourceActivated = nullptr;

    // Set up our callbacks
    setupCallbacks();
}

CECAdapter::~CECAdapter() {
    closeConnection();

    // Now reset the adapter pointer with lock
    {
        std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
        if (m_adapter) {
            LOG_INFO("Releasing CEC adapter resources");
            m_adapter.reset();
        }
    }

    // Clean up callbacks if we allocated them
    if (m_config.callbacks) {
        delete m_config.callbacks;
        m_config.callbacks = nullptr;
    }
}

void CECAdapter::populateConfigFromOptions(const Options& options) {
    m_options = options;

    // Set up device name
    snprintf(m_config.strDeviceName, sizeof(m_config.strDeviceName), "%s", m_options.deviceName.c_str());

    // Set up auto power on and wake AVR
    m_config.bAutoWakeAVR = m_options.autoWakeAVR ? 1 : 0;
    m_config.bAutoPowerOn = m_options.autoPowerOn ? 1 : 0;

    // Set up source activation and power off on standby
    m_config.bActivateSource = m_options.activateSource ? 1 : 0;
    m_config.bPowerOffOnStandby = m_options.powerOffOnStandby ? 1 : 0;

    // Set up wake devices and power off devices
    m_config.wakeDevices = m_options.wakeDevices;
    m_config.powerOffDevices = m_options.powerOffDevices;
}

bool CECAdapter::configureAdapter(const Options& options) {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);

    if (!m_adapter) {
        LOG_ERROR("Cannot configure adapter - not initialized");
        return false;
    }

    if (!m_connected) {
        LOG_ERROR("Cannot configure adapter - not connected");
        LOG_ERROR("Configuration changes require an active connection");
        return false;
    }

    LOG_INFO("Updating CEC adapter runtime configuration");
    LOG_INFO("Note: This is a runtime update, not initial configuration");

    populateConfigFromOptions(options);

    // Give adapter a moment to stabilize if recently connected
    // This is for runtime changes, should be rare
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!m_adapter->SetConfiguration(&m_config)) {
        LOG_ERROR("Failed to apply configuration to CEC adapter");
        LOG_ERROR("This may happen if adapter is not fully initialized");
        return false;
    }

    LOG_INFO("CEC adapter runtime configuration updated successfully");
    LOG_INFO("Changes will take effect immediately");

    return true;
}

void CECAdapter::setupCallbacks() {
    if (!m_config.callbacks) {
        LOG_ERROR("Cannot set up callbacks: callback structure is null");
        return;
    }

    try {
        // Set up callbacks with null checks
        m_config.callbacks->logMessage = CECAdapter::cecLogCallback;
        m_config.callbacks->commandReceived = CECAdapter::cecCommandCallback;
        m_config.callbacks->alert = CECAdapter::cecAlertCallback;

        m_config.callbackParam = this;

    } catch (const std::exception& e) {
        LOG_ERROR("Exception during callback setup: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception during callback setup");
    }
}

bool CECAdapter::detectAdapter() {
    LOG_INFO("Detecting CEC adapters...");
    CEC::cec_adapter_descriptor devices[10];
    int8_t numDevices = m_adapter->DetectAdapters(devices, 10, nullptr, true);

    if (numDevices <= 0) {
        LOG_ERROR("No CEC adapters found");
        return false;
    }

    LOG_INFO("Found ", static_cast<int>(numDevices), " CEC adapter(s)");
    m_portName = devices[0].strComName;
    LOG_INFO("Will use adapter: ", m_portName);
    return true;
}

bool CECAdapter::initialize() {
    LOG_INFO("Initializing libCEC");
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (m_adapter) {
        LOG_WARNING("libCEC already initialized");
        return true;
    }

    m_adapter = std::unique_ptr<CEC::ICECAdapter>(::CECInitialise(&m_config));
    if (!m_adapter) {
        LOG_ERROR("Failed to initialize libCEC - CECInitialise returned null");
        return false;
    }

    LOG_INFO("libCEC initialized, version ", m_adapter->VersionToString(m_config.clientVersion));
    LOG_INFO("Configuration passed to libCEC:");
    LOG_INFO("  Device name: '", m_config.strDeviceName, "'");
    LOG_INFO("  Device type: Playback Device");
    LOG_INFO("  Auto power on: ", m_config.bAutoPowerOn ? "enabled" : "disabled");
    LOG_INFO("  Auto wake AVR: ", m_config.bAutoWakeAVR ? "enabled" : "disabled");
    LOG_INFO("  Activate source: ", m_config.bActivateSource ? "enabled" : "disabled");
    LOG_INFO("  Power off on standby: ", m_config.bPowerOffOnStandby ? "enabled" : "disabled");
    LOG_INFO("This configuration will be applied automatically during Open() → RegisterClient()");

    if (!detectAdapter()) {
        m_adapter.reset();
        return false;
    }

    return true;
}

bool CECAdapter::openConnection() {
    LOG_INFO("Opening CEC adapter connection");
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);

    if (!m_adapter) {
        LOG_ERROR("Cannot open connection, libCEC not initialized");
        return false;
    }

    if (m_connected) {
        LOG_INFO("Connection already open");
        return true;
    }

    if (m_portName.empty()) {
        LOG_ERROR("No adapter port available - detection may have failed during initialization");
        return false;
    }

    try {
        // Open the adapter using the port detected during load
        LOG_INFO("Opening CEC adapter: ", m_portName);
        if (!m_adapter->Open(m_portName.c_str())) {
            LOG_ERROR("Failed to open CEC adapter");
            return false;
        }

        m_connected = true;

        // Configuration is automatically applied by libcec during internal client
        // registration (RegisterClient). The config we passed to CECInitialise()
        // in initialize() is used automatically during Open().
        //
        // DO NOT call SetConfiguration() here! It will be called too early
        // (before CECInitialised() is true) and will fail silently, causing
        // the OSD name to never be transmitted to the TV.
        //
        // See WHEN_TO_SET_CONFIGURATION.md for detailed explanation.
        LOG_INFO("Waiting for libcec client registration to complete (5-10 seconds)...");

        // Configure system audio mode
        if (!m_adapter->AudioEnable(m_options.systemAudioMode)) {
            LOG_WARNING("Failed to ", m_options.systemAudioMode ? "enable" : "disable", " system audio mode");

        } else {
            LOG_INFO("System audio mode ", m_options.systemAudioMode ? "enabled" : "disabled");
        }

        LOG_INFO("CEC adapter connection opened successfully");
        LOG_INFO("libcec is now performing client registration in background");
        LOG_INFO("This includes: allocating logical addresses, setting OSD name, and");
        LOG_INFO("transmitting device information to the CEC bus (takes 5-10 seconds)");
        LOG_INFO("OSD name verification will run in background via thread pool");

        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during CEC connection opening: ", e.what());
        m_connected = false;
        return false;
    }
    catch (...) {
        LOG_ERROR("Unknown exception during CEC connection opening");
        m_connected = false;
        return false;
    }
}

void CECAdapter::closeConnection() {
    if (!m_connected) {
        return;
    }

    // Set connected to false first to prevent new operations
    m_connected = false;

    // Use a timeout to prevent hanging on adapter close
    auto closeWithTimeout = [this]() -> bool {
        std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);

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
    if (closeFuture.wait_for(std::chrono::seconds(5)) == std::future_status::timeout) {
        LOG_WARNING("CEC adapter close operation timed out");
    }

    LOG_INFO("CEC adapter connection closed");
}

bool CECAdapter::reopenConnection(bool afterWake) {
    if (afterWake) {
        LOG_INFO("Reopening CEC adapter connection after system wake");
    } else {
        LOG_INFO("Reopening CEC adapter connection");
    }

    // Validate that adapter is loaded and we have port info
    {
        std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
        if (!m_adapter) {
            LOG_ERROR("Cannot reopen connection - libCEC not loaded");
            return false;
        }
        if (m_portName.empty()) {
            LOG_ERROR("Cannot reopen connection - no adapter port information available");
            return false;
        }
    }

    closeConnection();

    // Wait for system stabilization
    // After wake, USB hardware and CEC bus need more time to stabilize
    if (afterWake) {
        LOG_INFO("Waiting for hardware to stabilize after wake...");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return openConnection();
}

bool CECAdapter::isConnected() const {
    return m_connected;
}

CEC::ICECAdapter* CECAdapter::getRawAdapter() const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    return m_adapter.get();
}

bool CECAdapter::powerOnDevice(CEC::cec_logical_address address) {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->PowerOnDevices(address);
}

bool CECAdapter::standbyDevice(CEC::cec_logical_address address) {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->StandbyDevices(address);
}

bool CECAdapter::volumeUp() {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->VolumeUp();
}

bool CECAdapter::volumeDown() {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->VolumeDown();
}

bool CECAdapter::toggleMute() {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->AudioToggleMute();
}

bool CECAdapter::sendKeypress(CEC::cec_logical_address address, CEC::cec_user_control_code key, bool release) {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    if (release) {
        return m_adapter->SendKeyRelease(address);
    } else {
        return m_adapter->SendKeypress(address, key, false);
    }
}

uint16_t CECAdapter::getDevicePhysicalAddress(CEC::cec_logical_address address) const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return 0;

    return m_adapter->GetDevicePhysicalAddress(address);
}

bool CECAdapter::isDeviceActive(CEC::cec_logical_address address) const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->IsActiveDevice(address);
}

CEC::cec_power_status CECAdapter::getDevicePowerStatus(CEC::cec_logical_address address) const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return CEC::CEC_POWER_STATUS_UNKNOWN;

    return m_adapter->GetDevicePowerStatus(address);
}

std::string CECAdapter::getDeviceOSDName(CEC::cec_logical_address address) const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return "";

    return m_adapter->GetDeviceOSDName(address);
}

CEC::cec_logical_addresses CECAdapter::getActiveDevices() const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) {
        CEC::cec_logical_addresses empty;
        empty.Clear();
        return empty;
    }

    return m_adapter->GetActiveDevices();
}

CEC::cec_logical_address CECAdapter::getActiveSource() const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return CEC::CECDEVICE_UNKNOWN;

    return m_adapter->GetActiveSource();
}

bool CECAdapter::hasAdapter() const {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
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

    // Detect TV Standby command
    if (command->initiator == CEC::CECDEVICE_TV &&
        command->opcode == CEC::CEC_OPCODE_STANDBY) {

        LOG_INFO("TV power off command detected");

        // Check if auto-standby is enabled in config
        if (adapter->m_config.bPowerOffOnStandby) {
            LOG_INFO("TV powered off and auto-standby is enabled. Invoking callback.");
            if (adapter->m_tvStandbyCallback) {
                adapter->m_tvStandbyCallback();
            }
        }
    }
}

void CECAdapter::cecAlertCallback(void *cbParam, const CEC::libcec_alert alert, const CEC::libcec_parameter) {
    CECAdapter* adapter = static_cast<CECAdapter*>(cbParam);
    if (!adapter) return;

    switch(alert) {
        case CEC::CEC_ALERT_CONNECTION_LOST:
            LOG_ERROR("CEC connection lost");
            adapter->m_connected = false;
            if (adapter->m_connectionLostCallback) {
                adapter->m_connectionLostCallback();
            }
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

void CECAdapter::setAutoStandby(bool enabled) {
    m_config.bPowerOffOnStandby = enabled ? 1 : 0;
    LOG_INFO("Auto-standby feature ", enabled ? "enabled" : "disabled");

    // Also apply this change to the adapter
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (m_adapter) {
        if (!m_adapter->SetConfiguration(&m_config)) {
            LOG_WARNING("Failed to apply auto-standby setting to CEC adapter");
        }
    }
}

void CECAdapter::setOnTvStandbyCallback(std::function<void()> callback) {
    m_tvStandbyCallback = std::move(callback);
}

void CECAdapter::setConnectionLostCallback(std::function<void()> callback) {
    m_connectionLostCallback = std::move(callback);
}

bool CECAdapter::standbyDevices(CEC::cec_logical_address address) {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    // libCEC automatically uses powerOffDevices list when CECDEVICE_BROADCAST is used
    return m_adapter->StandbyDevices(address);
}

bool CECAdapter::powerOnDevices(CEC::cec_logical_address address) {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    // libCEC automatically uses wakeDevices list when CECDEVICE_BROADCAST is used
    return m_adapter->PowerOnDevices(address);
}

bool CECAdapter::verifyOSDNameRegistration(int maxAttempts) {
    std::lock_guard<std::recursive_mutex> lock(m_adapterMutex);

    if (!m_adapter || !m_connected) {
        LOG_ERROR("Cannot verify OSD name - adapter not connected");
        return false;
    }

    LOG_INFO("Verifying OSD name registration: '", m_config.strDeviceName, "'");
    LOG_INFO("Waiting for libcec RegisterClient() to complete...");

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        // First attempt waits longer - RegisterClient() takes 5-10 seconds
        // Subsequent attempts are quicker checks
        int waitMs = (attempt == 1) ? 7000 : 2000;
        std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));

        // Get our logical address
        CEC::cec_logical_addresses addresses = m_adapter->GetLogicalAddresses();
        if (addresses.IsEmpty()) {
            LOG_WARNING("Attempt ", attempt, "/", maxAttempts,
                       ": No logical addresses allocated yet");
            continue;
        }

        CEC::cec_logical_address myAddress = addresses.primary;
        LOG_DEBUG("Using logical address: ", static_cast<int>(myAddress));

        // Query the OSD name from adapter's perspective
        std::string registeredName = m_adapter->GetDeviceOSDName(myAddress);

        if (registeredName.empty()) {
            LOG_WARNING("Attempt ", attempt, "/", maxAttempts,
                       ": OSD name not yet set in adapter");
            continue;
        }

        if (registeredName == m_config.strDeviceName) {
            LOG_INFO("✓ OSD name verified successfully: '", registeredName, "'");
            return true;
        }

        // Name mismatch
        LOG_WARNING("Attempt ", attempt, "/", maxAttempts, ": OSD name mismatch");
        LOG_WARNING("  Expected: '", m_config.strDeviceName, "'");
        LOG_WARNING("  Got: '", registeredName, "'");
    }

    // Failed to verify after all attempts
    LOG_ERROR("✗ OSD name verification failed after ", maxAttempts, " attempts");
    LOG_ERROR("  Expected: '", m_config.strDeviceName, "'");
    LOG_ERROR("  Device may not appear with correct name on TV");
    LOG_ERROR("  Recommendation: Restart the daemon if device name is critical");

    return false;
}

} // namespace cec_control