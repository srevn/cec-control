#include "libcec_adapter.h"

#include "../../common/logger.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace cec_control {

namespace {

// Window for the USB subsystem to re-enumerate ttyACM* nodes after a
// Close() + CECDestroy() cycle. Matters most after suspend/wake, when
// the kernel's udev rebind is still in flight; empirically covers the
// settle time on the Pulse-Eight USB-CEC adapter. Longer is safe;
// shorter races the rebind and surfaces as a "no adapter found"
// DetectAdapters() result.
constexpr auto kUsbSettleDelay = std::chrono::milliseconds(500);

// Ceiling on libcec's port-open retry loop (Phase A of ICECAdapter::
// Open). libcec's default is 10s, which is useful on a cold start
// where the kernel driver is still binding but excessive on the
// reconnect path, where the same 10s worst case multiplies across the
// retry schedule. Healthy Pulse-Eight adapters complete the port open
// in <100ms; 2s covers unusually slow platforms with headroom.
constexpr uint32_t kConnectTimeoutMs = 2000;

} // namespace

LibCecAdapter::LibCecAdapter(AdapterConfig config, Callbacks callbacks)
    : m_config(std::move(config)),
      m_connected(false),
      m_observationCallback(std::move(callbacks.onObservation)),
      m_connectionLostCallback(std::move(callbacks.onConnectionLost)) {

    m_libcecConfig.Clear();
    m_libcecConfig.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
    m_libcecConfig.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);

    std::snprintf(m_libcecConfig.strDeviceName,
                  sizeof(m_libcecConfig.strDeviceName),
                  "%s", m_config.deviceName.c_str());
    m_libcecConfig.bAutoWakeAVR    = m_config.autoWakeAVR    ? 1 : 0;
    m_libcecConfig.bAutoPowerOn    = m_config.autoPowerOn    ? 1 : 0;
    // bPowerOffOnStandby is deliberately not mirrored here —
    // auto-standby is a dispatcher-level policy, gated on the
    // dispatcher's flag rather than libcec's internal config. Leaving
    // libcec's flag at its default (0) prevents the library from
    // taking its own standby-driven actions underneath us.
    m_libcecConfig.bActivateSource = m_config.activateSource ? 1 : 0;
    m_libcecConfig.wakeDevices     = m_config.wakeDevices;
    m_libcecConfig.powerOffDevices = m_config.powerOffDevices;

    // m_callbacks is value-initialised so every slot starts nullptr;
    // point libcec's config at it and install the handlers we care
    // about. libcec reads these from its internal threads without a
    // lock, which is safe because they are never reassigned after
    // construction.
    m_libcecConfig.callbacks    = &m_callbacks;
    m_libcecConfig.callbackParam = this;
    m_callbacks.logMessage      = &LibCecAdapter::cecLogCallback;
    m_callbacks.commandReceived = &LibCecAdapter::cecCommandCallback;
    m_callbacks.alert           = &LibCecAdapter::cecAlertCallback;
}

LibCecAdapter::~LibCecAdapter() {
    // closeConnection is idempotent; calling it here covers the case
    // where the adapter is destroyed without the worker's
    // close-on-exit having run (failed initialise, failed startup).
    closeConnection();
    if (m_adapter) {
        LOG_INFO("Releasing CEC adapter resources");
        m_adapter.reset();
    }
}

bool LibCecAdapter::detectAdapter() {
    LOG_INFO("Detecting CEC adapters...");
    CEC::cec_adapter_descriptor devices[10];
    const int8_t numDevices = m_adapter->DetectAdapters(devices, 10, nullptr, true);

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
    if (m_adapter) {
        LOG_WARNING("libCEC already initialized");
        return true;
    }

    m_adapter = AdapterPtr(::CECInitialise(&m_libcecConfig));
    if (!m_adapter) {
        LOG_ERROR("Failed to initialize libCEC - CECInitialise returned null");
        return false;
    }

    LOG_INFO("libCEC initialized, version ",
             m_adapter->VersionToString(m_libcecConfig.clientVersion));

    if (!detectAdapter()) {
        m_adapter.reset();
        return false;
    }

    return true;
}

bool LibCecAdapter::openConnection() {
    LOG_INFO("Opening CEC adapter connection");

    if (!m_adapter) {
        LOG_ERROR("Cannot open connection, libCEC not initialized");
        return false;
    }

    if (m_connected.load(std::memory_order_acquire)) {
        LOG_INFO("Connection already open");
        return true;
    }

    if (m_portName.empty()) {
        LOG_ERROR("No adapter port available");
        return false;
    }

    try {
        // Apply configuration to the adapter before opening.
        if (!m_adapter->SetConfiguration(&m_libcecConfig)) {
            LOG_WARNING("Failed to apply adapter configuration");
        }

        LOG_INFO("Opening CEC adapter: ", m_portName);
        if (!m_adapter->Open(m_portName.c_str(), kConnectTimeoutMs)) {
            LOG_ERROR("Failed to open CEC adapter");
            return false;
        }

        m_connected.store(true, std::memory_order_release);

        // Send SystemAudioModeRequest to the AVR when activating as
        // source. libcec's ActivateSource(500) uses a delayed path
        // that only handles TV-side messages (ActiveSource,
        // ImageViewOn) but skips the SystemAudioModeRequest to the
        // AVR. We send it explicitly here so the AVR knows to switch
        // its input to our HDMI port.
        //
        // Never call AudioEnable(false) — it sends a "terminate SAC"
        // message that disrupts audio if another device already
        // established SAC.
        if (m_config.activateSource || m_config.systemAudioMode) {
            if (!m_adapter->AudioEnable(true)) {
                LOG_WARNING("Failed to send SystemAudioModeRequest to AVR");
            } else {
                LOG_INFO("SystemAudioModeRequest sent to AVR");
            }
        }

        LOG_INFO("CEC adapter connection opened successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during CEC connection opening: ", e.what());
        m_connected.store(false, std::memory_order_release);
        return false;
    } catch (...) {
        LOG_ERROR("Unknown exception during CEC connection opening");
        m_connected.store(false, std::memory_order_release);
        return false;
    }
}

void LibCecAdapter::closeConnection() {
    if (!m_connected.load(std::memory_order_acquire)) {
        return;
    }
    m_connected.store(false, std::memory_order_release);

    if (!m_adapter) {
        return;
    }

    // Close is synchronous. A stuck libcec Close() here is caught by
    // the daemon's suspend-safety timer (inhibit lock is released at
    // 10s) or, on shutdown, by systemd's TimeoutStopSec / SIGKILL.
    // Spawning a supervising thread would only leak when Close()
    // truly hangs.
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
    LOG_INFO("Reopening CEC adapter connection");

    // A null m_adapter here means a prior reopen failed at the detect
    // step and reset it; skip the teardown (nothing to close, no USB
    // state to settle) and fall straight through to re-init + detect.
    // Without this guard the reconnect schedule degenerates into a
    // single useful attempt — every retry after the first would bail
    // at the top, and the FSM would abandon without ever trying again.
    //
    // When we do have a live instance, destroy-and-reinitialise is
    // mandatory: libcec (>= 7) binds per-client state to the
    // ICECAdapter instance that CECInitialise returned, and a
    // Close+Open cycle on the same instance leaves the internal
    // processor half-initialised — Open then reports "failed to
    // register a new CEC client: CEC processor is not initialised"
    // followed by a crash deep inside libcec. Destroy-and-reinitialise
    // is the pattern other libcec consumers use (kodi, cec-client,
    // libcec's own tests) and is what recovers cleanly after restart,
    // connection loss, and suspend/wake.
    if (m_adapter) {
        m_connected.store(false, std::memory_order_release);
        try {
            m_adapter->Close();
        } catch (const std::exception& e) {
            LOG_WARNING("Exception during adapter Close on reopen: ", e.what());
        }
        m_adapter.reset();  // invokes CECDestroy via the AdapterDeleter
        std::this_thread::sleep_for(kUsbSettleDelay);
    }

    m_adapter = AdapterPtr(::CECInitialise(&m_libcecConfig));
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
    return m_connected.load(std::memory_order_acquire);
}

bool LibCecAdapter::powerOnDevice(CEC::cec_logical_address address) {
    return callIfConnected(false, [&] { return m_adapter->PowerOnDevices(address); });
}

bool LibCecAdapter::standbyDevice(CEC::cec_logical_address address) {
    return callIfConnected(false, [&] { return m_adapter->StandbyDevices(address); });
}

bool LibCecAdapter::volumeUp() {
    return callIfConnected(false, [&] { return m_adapter->VolumeUp(); });
}

bool LibCecAdapter::volumeDown() {
    return callIfConnected(false, [&] { return m_adapter->VolumeDown(); });
}

bool LibCecAdapter::toggleMute() {
    return callIfConnected(false, [&] { return m_adapter->AudioToggleMute(); });
}

bool LibCecAdapter::sendKeypress(CEC::cec_logical_address address,
                                  CEC::cec_user_control_code key,
                                  bool release) {
    return callIfConnected(false, [&] {
        if (release) return m_adapter->SendKeyRelease(address);
        return m_adapter->SendKeypress(address, key, false);
    });
}

bool LibCecAdapter::setStreamPath(uint16_t physicalAddress) {
    return callIfConnected(false, [&] { return m_adapter->SetStreamPath(physicalAddress); });
}

uint16_t LibCecAdapter::getDevicePhysicalAddress(CEC::cec_logical_address address) const {
    return callIfConnected(uint16_t{0},
        [&] { return m_adapter->GetDevicePhysicalAddress(address); });
}

bool LibCecAdapter::isDeviceActive(CEC::cec_logical_address address) const {
    return callIfConnected(false, [&] { return m_adapter->IsActiveDevice(address); });
}

CEC::cec_power_status LibCecAdapter::getDevicePowerStatus(CEC::cec_logical_address address) const {
    return callIfConnected(CEC::CEC_POWER_STATUS_UNKNOWN,
        [&] { return m_adapter->GetDevicePowerStatus(address); });
}

std::string LibCecAdapter::getDeviceOSDName(CEC::cec_logical_address address) const {
    return callIfConnected(std::string{},
        [&] { return m_adapter->GetDeviceOSDName(address); });
}

CEC::cec_logical_addresses LibCecAdapter::getActiveDevices() const {
    CEC::cec_logical_addresses empty;
    empty.Clear();
    return callIfConnected(empty, [&] { return m_adapter->GetActiveDevices(); });
}

CEC::cec_logical_address LibCecAdapter::getActiveSource() const {
    return callIfConnected(CEC::CECDEVICE_UNKNOWN,
        [&] { return m_adapter->GetActiveSource(); });
}

bool LibCecAdapter::standbyDevices(CEC::cec_logical_address address) {
    // libcec automatically uses powerOffDevices list when
    // CECDEVICE_BROADCAST is used.
    return callIfConnected(false, [&] { return m_adapter->StandbyDevices(address); });
}

bool LibCecAdapter::powerOnDevices(CEC::cec_logical_address address) {
    // libcec automatically uses wakeDevices list when
    // CECDEVICE_BROADCAST is used.
    return callIfConnected(false, [&] { return m_adapter->PowerOnDevices(address); });
}

// libcec callback trampolines ----------------------------------------

void LibCecAdapter::cecLogCallback(void* cbParam,
                                    const CEC::cec_log_message* message) {
    auto* adapter = static_cast<LibCecAdapter*>(cbParam);
    if (!adapter || !message) return;

    LogLevel level;
    switch (message->level) {
        case CEC::CEC_LOG_ERROR:   level = LogLevel::ERROR;   break;
        case CEC::CEC_LOG_WARNING: level = LogLevel::WARNING; break;
        case CEC::CEC_LOG_NOTICE:  level = LogLevel::INFO;    break;
        case CEC::CEC_LOG_TRAFFIC: level = LogLevel::TRAFFIC; break;
        case CEC::CEC_LOG_DEBUG:   level = LogLevel::DEBUG;   break;
        default:                   level = LogLevel::INFO;    break;
    }

    Logger::getInstance().log(level, "CEC: ", message->message);
}

void LibCecAdapter::cecCommandCallback(void* cbParam,
                                        const CEC::cec_command* command) {
    auto* adapter = static_cast<LibCecAdapter*>(cbParam);
    if (!adapter || !command) return;

    LOG_DEBUG("CEC command received: initiator=",
              static_cast<int>(command->initiator),
              ", destination=", static_cast<int>(command->destination),
              ", opcode=", static_cast<int>(command->opcode));

    if (!adapter->m_observationCallback) return;

    // A small set of opcodes is surfaced to the daemon as typed
    // Observations; every other bus command is ignored here. Keeping
    // the filter narrow is the whole point of doing it on this thread:
    // daemon-side subscribers run single-threaded on the main loop and
    // should not need to re-filter the stream.
    //
    // ACTIVE_SOURCE, ROUTING_CHANGE and SET_STREAM_PATH all collapse
    // to Observation::Kind::ActiveSource — each announces the new
    // active source in a different way, and the hook subsystem's
    // dedup collapses any redundant back-to-back emissions. We have
    // to watch all three: when a routing change makes *this* host the
    // active source, libcec sends ACTIVE_SOURCE on our behalf, but
    // that outgoing frame never reaches commandReceived — only the
    // upstream ROUTING_CHANGE / SET_STREAM_PATH does.
    const auto& params = command->parameters;

    auto emitActiveSource = [&](std::size_t offset) {
        Observation obs;
        obs.kind = Observation::Kind::ActiveSource;
        // Physical address is 16 bits, big-endian on the wire.
        obs.physicalAddress = static_cast<uint16_t>(
            (static_cast<uint16_t>(params.data[offset])     << 8) |
             static_cast<uint16_t>(params.data[offset + 1]));
        adapter->m_observationCallback(obs);
    };

    if (command->initiator == CEC::CECDEVICE_TV &&
        command->opcode == CEC::CEC_OPCODE_STANDBY) {
        LOG_INFO("TV standby opcode observed");
        Observation obs;
        obs.kind = Observation::Kind::TvStandby;
        adapter->m_observationCallback(obs);
        return;
    }

    if (command->initiator == CEC::CECDEVICE_TV &&
        command->opcode == CEC::CEC_OPCODE_REPORT_POWER_STATUS &&
        params.size >= 1) {
        Observation obs;
        obs.kind  = Observation::Kind::TvPowerReport;
        obs.power = static_cast<CEC::cec_power_status>(params.data[0]);
        adapter->m_observationCallback(obs);
        return;
    }

    if (command->opcode == CEC::CEC_OPCODE_ACTIVE_SOURCE &&
        params.size >= 2) {
        emitActiveSource(0);
        return;
    }

    // ROUTING_CHANGE payload: old address in [0..1], new address in
    // [2..3]; we only care about the new path.
    if (command->opcode == CEC::CEC_OPCODE_ROUTING_CHANGE &&
        params.size >= 4) {
        emitActiveSource(2);
        return;
    }

    // SET_STREAM_PATH payload: new active address in [0..1].
    if (command->opcode == CEC::CEC_OPCODE_SET_STREAM_PATH &&
        params.size >= 2) {
        emitActiveSource(0);
        return;
    }
}

void LibCecAdapter::cecAlertCallback(void* cbParam,
                                      const CEC::libcec_alert alert,
                                      const CEC::libcec_parameter) {
    auto* adapter = static_cast<LibCecAdapter*>(cbParam);
    if (!adapter) return;

    switch (alert) {
    case CEC::CEC_ALERT_CONNECTION_LOST:
        LOG_ERROR("CEC connection lost");
        // libcec's alert thread signals lost-link here. The atomic
        // write is the only cross-thread field on the adapter; the
        // follow-up callback is install-once, so it can be fired
        // directly without coordination.
        adapter->m_connected.store(false, std::memory_order_release);
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

} // namespace cec_control
