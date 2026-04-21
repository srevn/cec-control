#include "libcec_adapter.h"
#include "../../common/logger.h"

#include <thread>
#include <unistd.h>
#include <sys/types.h>

namespace cec_control {

LibCecAdapter::LibCecAdapter(Options options, Callbacks callbacks)
    : m_options(std::move(options)),
      m_connected(false),
      m_tvStandbyCallback(std::move(callbacks.onTvStandby)),
      m_connectionLostCallback(std::move(callbacks.onConnectionLost)) {

    // Initialize libcec configuration
    m_config.Clear();
    m_config.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
    m_config.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);

    // Populate config from options
    populateConfigFromOptions(m_options);

    // m_callbacks is value-initialised so every slot is nullptr; point
    // libcec's config at it and let setupCallbacks() install the handlers
    // we care about.
    m_config.callbacks = &m_callbacks;
    setupCallbacks();
}

LibCecAdapter::~LibCecAdapter() {
    closeConnection();

    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (m_adapter) {
        LOG_INFO("Releasing CEC adapter resources");
        m_adapter.reset();
    }
}

void LibCecAdapter::populateConfigFromOptions(const Options& options) {
    m_options = options;

    // Set up device name
    snprintf(
        m_config.strDeviceName, sizeof(m_config.strDeviceName),
        "%s", m_options.deviceName.c_str()
    );

    // Set up auto power on and wake AVR
    m_config.bAutoWakeAVR = m_options.autoWakeAVR ? 1 : 0;
    m_config.bAutoPowerOn = m_options.autoPowerOn ? 1 : 0;

    // Source activation. bPowerOffOnStandby is deliberately not mirrored here
    // — auto-standby is a router-level policy, gated on the router's flag
    // rather than libcec's internal config. Leaving libcec's flag at its
    // default (0) prevents the library from taking its own standby-driven
    // actions underneath us.
    m_config.bActivateSource = m_options.activateSource ? 1 : 0;

    // Set up wake devices and power off devices
    m_config.wakeDevices = m_options.wakeDevices;
    m_config.powerOffDevices = m_options.powerOffDevices;
}

void LibCecAdapter::setupCallbacks() {
    if (!m_config.callbacks) {
        LOG_ERROR("Cannot set up callbacks: callback structure is null");
        return;
    }

    try {
        // Set up callbacks with null checks
        m_config.callbacks->logMessage = LibCecAdapter::cecLogCallback;
        m_config.callbacks->commandReceived = LibCecAdapter::cecCommandCallback;
        m_config.callbacks->alert = LibCecAdapter::cecAlertCallback;

        m_config.callbackParam = this;

    } catch (const std::exception& e) {
        LOG_ERROR("Exception during callback setup: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception during callback setup");
    }
}

bool LibCecAdapter::detectAdapter() {
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

bool LibCecAdapter::initialize() {
    LOG_INFO("Initializing libCEC");
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (m_adapter) {
        LOG_WARNING("libCEC already initialized");
        return true;
    }

    m_adapter = AdapterPtr(::CECInitialise(&m_config));
    if (!m_adapter) {
        LOG_ERROR("Failed to initialize libCEC - CECInitialise returned null");
        return false;
    }

    LOG_INFO("libCEC initialized, version ",
             m_adapter->VersionToString(m_config.clientVersion));

    if (!detectAdapter()) {
        m_adapter.reset();
        return false;
    }

    return true;
}

bool LibCecAdapter::openConnection() {
    LOG_INFO("Opening CEC adapter connection");
    std::lock_guard<std::mutex> lock(m_adapterMutex);

    if (!m_adapter) {
        LOG_ERROR("Cannot open connection, libCEC not initialized");
        return false;
    }

    if (m_connected) {
        LOG_INFO("Connection already open");
        return true;
    }

    if (m_portName.empty()) {
        LOG_ERROR("No adapter port available");
        return false;
    }

    try {
        // Apply configuration to the adapter before opening
        if (!m_adapter->SetConfiguration(&m_config)) {
            LOG_WARNING("Failed to apply adapter configuration");
        }

        // Open the adapter using the port detected during initialization/reopen
        LOG_INFO("Opening CEC adapter: ", m_portName);
        if (!m_adapter->Open(m_portName.c_str())) {
            LOG_ERROR("Failed to open CEC adapter");
            return false;
        }

        m_connected = true;

        // Send SystemAudioModeRequest to the AVR when activating as source.
        // libCEC's ActivateSource(500) uses a delayed path that only handles
        // TV-side messages (ActiveSource, ImageViewOn) but skips the
        // SystemAudioModeRequest to the AVR. We send it explicitly here so
        // the AVR knows to switch its input to our HDMI port.
        //
        // Never call AudioEnable(false) — it sends a "terminate SAC" message
        // that disrupts audio if another device already established SAC.
        if (m_options.activateSource || m_options.systemAudioMode) {
            if (!m_adapter->AudioEnable(true)) {
                LOG_WARNING("Failed to send SystemAudioModeRequest to AVR");
            } else {
                LOG_INFO("SystemAudioModeRequest sent to AVR");
            }
        }

        LOG_INFO("CEC adapter connection opened successfully");

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

void LibCecAdapter::closeConnection() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_connected) {
        return;
    }
    m_connected = false;

    if (!m_adapter) {
        return;
    }

    // Close is synchronous. A stuck libcec Close() here is caught by the
    // daemon's suspend-safety timer (inhibit lock is released at 10s) or,
    // on shutdown, by systemd's TimeoutStopSec / SIGKILL. Spawning a
    // supervising thread would only leak when Close() truly hangs.
    try {
        LOG_INFO("Setting inactive view before close");
        m_adapter->SetInactiveView();
        LOG_INFO("Closing CEC adapter connection");
        m_adapter->Close();
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during CEC adapter close: ", e.what());
    } catch (...) {
        LOG_ERROR("Unknown exception during CEC adapter close");
    }

    LOG_INFO("CEC adapter connection closed");
}

bool LibCecAdapter::reopenConnection() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    LOG_INFO("Reopening CEC adapter connection");

    if (!m_adapter) {
        LOG_ERROR("Cannot reopen connection - libCEC not loaded");
        return false;
    }

    // Full teardown and re-initialisation: libcec (>= 7) binds per-client state
    // to the ICECAdapter instance that CECInitialise returned, and a
    // Close+Open cycle on the same instance leaves the internal processor
    // half-initialised — Open then reports "failed to register a new CEC
    // client: CEC processor is not initialised" followed by a crash deep
    // inside libcec. Destroy-and-reinitialise is the pattern other libcec
    // consumers use (kodi, cec-client, libcec's own tests) and is what
    // recovers cleanly after `restart`, connection loss, and suspend/wake.
    m_connected = false;
    try {
        m_adapter->Close();
    } catch (const std::exception& e) {
        LOG_WARNING("Exception during adapter Close on reopen: ", e.what());
    }
    m_adapter.reset();  // invokes CECDestroy via the AdapterDeleter

    // Let the USB subsystem settle before the fresh Initialise+Detect+Open
    // cycle — matters after sleep/wake when ttyACM* may still be rebinding.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    m_adapter = AdapterPtr(::CECInitialise(&m_config));
    if (!m_adapter) {
        LOG_ERROR("Failed to re-initialise libCEC on reopen");
        return false;
    }

    if (!detectAdapter()) {
        LOG_ERROR("Failed to detect adapter during reopen");
        m_adapter.reset();
        return false;
    }

    return openConnection();
}

bool LibCecAdapter::isConnected() const {
    return m_connected;
}

bool LibCecAdapter::powerOnDevice(CEC::cec_logical_address address) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->PowerOnDevices(address);
}

bool LibCecAdapter::standbyDevice(CEC::cec_logical_address address) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->StandbyDevices(address);
}

bool LibCecAdapter::volumeUp() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->VolumeUp();
}

bool LibCecAdapter::volumeDown() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->VolumeDown();
}

bool LibCecAdapter::toggleMute() {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->AudioToggleMute();
}

bool LibCecAdapter::sendKeypress(CEC::cec_logical_address address, CEC::cec_user_control_code key, bool release) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    if (release) {
        return m_adapter->SendKeyRelease(address);
    } else {
        return m_adapter->SendKeypress(address, key, false);
    }
}

bool LibCecAdapter::setStreamPath(uint16_t physicalAddress) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->SetStreamPath(physicalAddress);
}

uint16_t LibCecAdapter::getDevicePhysicalAddress(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return 0;

    return m_adapter->GetDevicePhysicalAddress(address);
}

bool LibCecAdapter::isDeviceActive(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    return m_adapter->IsActiveDevice(address);
}

CEC::cec_power_status LibCecAdapter::getDevicePowerStatus(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return CEC::CEC_POWER_STATUS_UNKNOWN;

    return m_adapter->GetDevicePowerStatus(address);
}

std::string LibCecAdapter::getDeviceOSDName(CEC::cec_logical_address address) const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return "";

    return m_adapter->GetDeviceOSDName(address);
}

CEC::cec_logical_addresses LibCecAdapter::getActiveDevices() const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) {
        CEC::cec_logical_addresses empty;
        empty.Clear();
        return empty;
    }

    return m_adapter->GetActiveDevices();
}

CEC::cec_logical_address LibCecAdapter::getActiveSource() const {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return CEC::CECDEVICE_UNKNOWN;

    return m_adapter->GetActiveSource();
}

// Callback implementations
void LibCecAdapter::cecLogCallback(void *cbParam, const CEC::cec_log_message* message) {
    LibCecAdapter* adapter = static_cast<LibCecAdapter*>(cbParam);
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

void LibCecAdapter::cecCommandCallback(void *cbParam, const CEC::cec_command* command) {
    LibCecAdapter* adapter = static_cast<LibCecAdapter*>(cbParam);
    if (!adapter || !command) return;

    // Log received commands
    LOG_DEBUG("CEC command received: initiator=", static_cast<int>(command->initiator),
              ", destination=", static_cast<int>(command->destination),
              ", opcode=", static_cast<int>(command->opcode));

    // Detect TV Standby command. Fire the callback unconditionally; the
    // caller owns the policy decision (whether to act on the signal).
    if (command->initiator == CEC::CECDEVICE_TV &&
        command->opcode == CEC::CEC_OPCODE_STANDBY) {
        LOG_INFO("TV power off command detected");
        if (adapter->m_tvStandbyCallback) {
            adapter->m_tvStandbyCallback();
        }
    }
}

void LibCecAdapter::cecAlertCallback(void *cbParam, const CEC::libcec_alert alert, const CEC::libcec_parameter) {
    LibCecAdapter* adapter = static_cast<LibCecAdapter*>(cbParam);
    if (!adapter) return;

    switch(alert) {
        case CEC::CEC_ALERT_CONNECTION_LOST:
            LOG_ERROR("CEC connection lost");
            // Unlocked write from libcec's internal thread — see the
            // m_connected invariant in libcec_adapter.h. Taking
            // m_adapterMutex here would risk deadlock if another thread
            // is already inside a libcec call under the same mutex.
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

bool LibCecAdapter::standbyDevices(CEC::cec_logical_address address) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    // libCEC automatically uses powerOffDevices list when CECDEVICE_BROADCAST is used
    return m_adapter->StandbyDevices(address);
}

bool LibCecAdapter::powerOnDevices(CEC::cec_logical_address address) {
    std::lock_guard<std::mutex> lock(m_adapterMutex);
    if (!m_adapter || !m_connected) return false;

    // libCEC automatically uses wakeDevices list when CECDEVICE_BROADCAST is used
    return m_adapter->PowerOnDevices(address);
}

} // namespace cec_control