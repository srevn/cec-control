#include "command_router.h"
#include "../common/command_registry.h"
#include "../common/logger.h"

#include <chrono>
#include <ios>
#include <thread>
#include <utility>

namespace cec_control {

namespace {

// HDMI source IDs as documented in --help: 2..5 map to HDMI 1..4. The CEC
// physical address byte layout is 0xN000 where N is the HDMI port number.
constexpr uint8_t kFirstHdmiSource = 2;
constexpr uint8_t kLastHdmiSource  = 5;

uint16_t hdmiPhysicalAddress(uint8_t source) noexcept {
    return static_cast<uint16_t>((source - kFirstHdmiSource + 1) << 12);
}

CEC::cec_user_control_code hdmiNumberKey(uint8_t source) noexcept {
    switch (source) {
        case 2: return CEC::CEC_USER_CONTROL_CODE_NUMBER1;
        case 3: return CEC::CEC_USER_CONTROL_CODE_NUMBER2;
        case 4: return CEC::CEC_USER_CONTROL_CODE_NUMBER3;
        case 5: return CEC::CEC_USER_CONTROL_CODE_NUMBER4;
    }
    return CEC::CEC_USER_CONTROL_CODE_UNKNOWN;
}

bool powerOnDevice(CECAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Powering on device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter, logicalAddress]() {
        const auto addr = static_cast<CEC::cec_logical_address>(logicalAddress);
        if (!adapter.isDeviceActive(addr)) {
            LOG_WARNING("Device ", static_cast<int>(logicalAddress), " is not active");
        }
        return adapter.powerOnDevice(addr);
    });
}

bool powerOffDevice(CECAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Powering off device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter, logicalAddress]() {
        const auto addr = static_cast<CEC::cec_logical_address>(logicalAddress);
        if (!adapter.isDeviceActive(addr)) {
            LOG_WARNING("Device ", static_cast<int>(logicalAddress), " is not active");
        }
        return adapter.standbyDevice(addr);
    });
}

bool setVolume(CECAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress, bool up) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Setting volume ", up ? "up" : "down",
             " on device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter, up]() {
        return up ? adapter.volumeUp() : adapter.volumeDown();
    });
}

bool setMute(CECAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress, bool mute) {
    if (!adapter.isConnected()) return false;
    LOG_INFO(mute ? "Muting" : "Unmuting", " device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter]() {
        return adapter.toggleMute();
    });
}

bool setSource(CECAdapter& adapter, CommandThrottler& throttler,
               uint8_t /*logicalAddress*/, uint8_t source) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Selecting input source ", static_cast<int>(source), " on TV");

    return throttler.executeWithThrottle([&adapter, source]() {
        // Sources 0 and 1 are TV-internal inputs without a CEC physical
        // address; SetStreamPath cannot reach them, so go straight to a
        // function-key keypress.
        auto sendKey = [&adapter](CEC::cec_user_control_code key) {
            if (!adapter.sendKeypress(CEC::CECDEVICE_TV, key, false)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            adapter.sendKeypress(CEC::CECDEVICE_TV, CEC::CEC_USER_CONTROL_CODE_UNKNOWN, true);
            return true;
        };

        if (source == 0) return sendKey(CEC::CEC_USER_CONTROL_CODE_SELECT_AV_INPUT_FUNCTION);
        if (source == 1) return sendKey(CEC::CEC_USER_CONTROL_CODE_SELECT_AUDIO_INPUT_FUNCTION);
        if (source < kFirstHdmiSource || source > kLastHdmiSource) {
            LOG_WARNING("Invalid source value: ", source);
            return false;
        }

        // HDMI input: SetStreamPath is the canonical mechanism; fall back to
        // INPUT_SELECT + number keypress sequence if the TV refuses.
        const uint16_t physicalAddress = hdmiPhysicalAddress(source);
        LOG_INFO("Setting stream path to physical address: 0x",
                 std::hex, physicalAddress);

        if (adapter.setStreamPath(physicalAddress)) {
            return true;
        }

        LOG_INFO("SetStreamPath failed, trying with key presses");
        if (!adapter.sendKeypress(CEC::CECDEVICE_TV,
                                  CEC::CEC_USER_CONTROL_CODE_INPUT_SELECT, false)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!adapter.sendKeypress(CEC::CECDEVICE_TV, hdmiNumberKey(source), false)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        adapter.sendKeypress(CEC::CECDEVICE_TV, CEC::CEC_USER_CONTROL_CODE_UNKNOWN, true);
        return true;
    });
}

void logDeviceSnapshot(CECAdapter& adapter) {
    try {
        const CEC::cec_logical_addresses addresses = adapter.getActiveDevices();

        int activeCount = 0;
        for (int i = 0; i < 16; ++i) {
            if (addresses[i]) ++activeCount;
        }
        LOG_INFO("Found ", activeCount, " active CEC device(s)");

        LOG_INFO("Scanning for CEC devices power status...");
        for (int i = 0; i < 15; ++i) {
            const auto cecAddress = static_cast<CEC::cec_logical_address>(i);
            const char* status = "unknown";
            switch (adapter.getDevicePowerStatus(cecAddress)) {
                case CEC::CEC_POWER_STATUS_ON:                          status = "ON"; break;
                case CEC::CEC_POWER_STATUS_STANDBY:                     status = "STANDBY"; break;
                case CEC::CEC_POWER_STATUS_IN_TRANSITION_STANDBY_TO_ON: status = "TURNING ON"; break;
                case CEC::CEC_POWER_STATUS_IN_TRANSITION_ON_TO_STANDBY: status = "TURNING OFF"; break;
                default: break;
            }
            LOG_INFO("Device ", i, ": Power status = ", status);
        }

        const CEC::cec_logical_address active = adapter.getActiveSource();
        if (active != CEC::CECDEVICE_UNKNOWN) {
            LOG_INFO("Active source: Device ", static_cast<int>(active));
        } else {
            LOG_INFO("No active source detected");
        }

        for (int i = 0; i < 16; ++i) {
            if (!addresses[i]) continue;
            const auto addr = static_cast<CEC::cec_logical_address>(i);
            LOG_INFO("Device ", i, ": ", adapter.getDeviceOSDName(addr),
                     " (", adapter.isDeviceActive(addr) ? "active" : "inactive", ")");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during device scanning: ", e.what());
    }
}

} // namespace

CommandRouter::CommandRouter(Options options,
                             std::shared_ptr<ThreadPool> threadPool,
                             Callbacks callbacks)
    // TV standby always lifts to our onTvStandby hook; policy (whether to
    // suspend the PC) lives here in the router, not in the adapter. That
    // way CMD_AUTO_STANDBY toggles a single flag with no libcec config
    // touch. The [this] capture is legal during initializer-list
    // evaluation — the lambda stores the pointer, it does not invoke
    // onTvStandby until libcec fires a callback long after construction.
    : m_adapter(options.adapter, CECAdapter::Callbacks{
          /*onTvStandby*/      [this]() { onTvStandby(); },
          /*onConnectionLost*/ std::move(callbacks.onConnectionLost),
      }),
      m_throttler(options.throttler),
      m_options(std::move(options)),
      m_threadPool(std::move(threadPool)),
      m_autoStandbyEnabled(m_options.autoStandbyEnabled),
      m_suspendCallback(std::move(callbacks.onSuspendRequested)) {}

CommandRouter::~CommandRouter() {
    shutdown();
}

bool CommandRouter::initialize() {
    std::lock_guard<std::mutex> lock(m_routerMutex);
    LOG_INFO("Initializing CEC command router");

    if (!m_adapter.initialize()) {
        LOG_ERROR("Failed to initialize CEC adapter library");
        return false;
    }
    if (!m_adapter.openConnection()) {
        LOG_ERROR("Failed to open CEC adapter connection");
        return false;
    }

    if (m_options.scanDevicesAtStartup) {
        LOG_INFO("Scanning for CEC devices...");
        logDeviceSnapshot(m_adapter);
    } else {
        LOG_INFO("Skipping device scanning");
    }

    LOG_INFO("CEC command router initialized successfully");
    return true;
}

void CommandRouter::shutdown() {
    std::lock_guard<std::mutex> lock(m_routerMutex);
    if (m_shutdownComplete) return;
    m_shutdownComplete = true;

    LOG_INFO("Shutting down CEC command router");
    m_adapter.closeConnection();
    if (!m_queuedCommands.empty()) {
        LOG_INFO("Discarding ", m_queuedCommands.size(), " queued commands on shutdown");
        m_queuedCommands.clear();
    }
}

bool CommandRouter::reconnect() {
    std::lock_guard<std::mutex> lock(m_routerMutex);

    if (m_suspended) {
        // The adapter is intentionally closed during suspend; libCEC won't
        // see a connection-lost event here, so this branch mostly fires when
        // a stray alert lands between suspend() and the kernel actually
        // suspending. No-op and wait for resume() to reopen.
        LOG_DEBUG("reconnect() called while suspended; ignoring");
        return false;
    }

    if (isAdapterValid()) {
        LOG_DEBUG("reconnect(): adapter already connected");
        return true;
    }

    LOG_INFO("Attempting to reconnect to CEC adapter");
    if (!m_adapter.reopenConnection()) {
        LOG_ERROR("Failed to reconnect CEC adapter");
        return false;
    }
    LOG_INFO("CEC adapter reconnected successfully");
    return true;
}

void CommandRouter::suspend() {
    std::lock_guard<std::mutex> lock(m_routerMutex);
    if (m_suspended) {
        LOG_DEBUG("suspend() called while already suspended");
        return;
    }
    m_suspended = true;

    LOG_INFO("Preparing CEC adapter for system sleep");
    if (m_adapter.isConnected()) {
        try {
            m_adapter.standbyDevices(CEC::CECDEVICE_BROADCAST);
        } catch (const std::exception& e) {
            LOG_ERROR("Exception sending standby commands: ", e.what());
        }
    }
    m_adapter.closeConnection();
    LOG_INFO("CEC adapter closed for suspend");
}

void CommandRouter::resume() {
    std::lock_guard<std::mutex> lock(m_routerMutex);
    if (!m_suspended) {
        LOG_DEBUG("resume() called while not suspended");
        return;
    }

    LOG_INFO("Reinitializing CEC adapter after resume");
    const bool reconnected = m_adapter.reopenConnection();
    if (reconnected) {
        LOG_INFO("CEC adapter reconnected successfully on resume");
        try {
            m_adapter.powerOnDevices(CEC::CECDEVICE_BROADCAST);
        } catch (const std::exception& e) {
            LOG_ERROR("Exception sending power-on commands: ", e.what());
        }
    } else {
        LOG_ERROR("Failed to reconnect CEC adapter on resume");
    }

    // Flip the flag before draining so drained commands take the normal
    // (non-queued) path. New dispatches arriving while we still hold the lock
    // will be serialised *after* the drain completes, preserving the "queued
    // during suspend → run first" contract.
    m_suspended = false;

    if (!reconnected) {
        // Commands already got RESP_SUCCESS when queued ("accepted for
        // post-resume"). We can't deliver on that promise, so log and drop.
        if (!m_queuedCommands.empty()) {
            LOG_WARNING("Discarding ", m_queuedCommands.size(),
                        " queued commands: reconnect failed");
            m_queuedCommands.clear();
        }
        return;
    }

    if (m_queuedCommands.empty()) return;

    std::vector<Message> toDrain = std::move(m_queuedCommands);
    m_queuedCommands.clear();
    LOG_INFO("Processing ", toDrain.size(), " queued commands");
    for (const auto& cmd : toDrain) {
        try {
            (void)dispatchLocked(cmd);
        } catch (const std::exception& e) {
            LOG_ERROR("Exception processing queued command: ", e.what());
        }
    }
}

Message CommandRouter::dispatch(const Message& command) {
    std::lock_guard<std::mutex> lock(m_routerMutex);
    return dispatchLocked(command);
}

Message CommandRouter::dispatchLocked(const Message& command) {
    // CMD_RESTART_ADAPTER bypasses both the suspend and connected checks —
    // it's the operator's override to force a fresh adapter, and the
    // reconnect itself runs asynchronously on a pool worker.
    if (command.type == MessageType::CMD_RESTART_ADAPTER) {
        LOG_INFO("Scheduling adapter restart");
        scheduleRestart();
        return Message(MessageType::RESP_SUCCESS);
    }

    if (m_suspended) {
        const auto* spec = findByType(command.type);
        if (m_options.queueCommandsDuringSuspend &&
            spec && spec->queueableWhileSuspended) {
            m_queuedCommands.push_back(command);
            LOG_INFO("Queued command type=", static_cast<int>(command.type),
                     " for execution after resume");
            return Message(MessageType::RESP_SUCCESS);
        }
        LOG_WARNING("Command type=", static_cast<int>(command.type),
                    " received while suspended and cannot be queued");
        return Message(MessageType::RESP_ERROR);
    }

    if (!isAdapterValid()) {
        LOG_ERROR("Cannot process command: CEC adapter not connected");
        return Message(MessageType::RESP_ERROR);
    }

    bool success = false;
    switch (command.type) {
        case MessageType::CMD_VOLUME_UP:
            success = cec_control::setVolume(m_adapter, m_throttler, command.deviceId, true);
            break;
        case MessageType::CMD_VOLUME_DOWN:
            success = cec_control::setVolume(m_adapter, m_throttler, command.deviceId, false);
            break;
        case MessageType::CMD_VOLUME_MUTE:
            success = cec_control::setMute(m_adapter, m_throttler, command.deviceId, true);
            break;
        case MessageType::CMD_POWER_ON:
            success = cec_control::powerOnDevice(m_adapter, m_throttler, command.deviceId);
            break;
        case MessageType::CMD_POWER_OFF:
            success = cec_control::powerOffDevice(m_adapter, m_throttler, command.deviceId);
            break;
        case MessageType::CMD_CHANGE_SOURCE:
            if (!command.data.empty()) {
                success = cec_control::setSource(m_adapter, m_throttler,
                                                  command.deviceId, command.data[0]);
            }
            break;
        case MessageType::CMD_AUTO_STANDBY:
            if (!command.data.empty()) {
                const bool enabled = command.data[0] > 0;
                m_autoStandbyEnabled.store(enabled, std::memory_order_release);
                LOG_INFO("Auto-standby ", enabled ? "enabled" : "disabled");
                success = true;
            }
            break;
        default:
            LOG_ERROR("Unknown command type: ", static_cast<int>(command.type));
            return Message(MessageType::RESP_ERROR);
    }

    return success ? Message(MessageType::RESP_SUCCESS)
                   : Message(MessageType::RESP_ERROR);
}

void CommandRouter::scheduleRestart() {
    m_threadPool->submit([this]() {
        std::lock_guard<std::mutex> lock(m_routerMutex);
        if (m_adapter.reopenConnection()) {
            LOG_INFO("Adapter restart completed successfully");
        } else {
            LOG_ERROR("Failed to restart adapter");
        }
    });
}

void CommandRouter::onTvStandby() {
    // Runs on libCEC's internal thread. Must not touch m_routerMutex — a
    // concurrent dispatch may hold it and libcec does not document whether
    // its callback thread can be blocked indefinitely.
    if (!m_autoStandbyEnabled.load(std::memory_order_acquire)) {
        LOG_DEBUG("TV standby observed; auto-standby disabled — ignoring");
        return;
    }

    LOG_INFO("TV standby observed with auto-standby enabled; initiating system suspend");

    m_threadPool->submit([this]() {
        if (!m_suspendCallback) {
            LOG_WARNING("No suspend callback wired; cannot suspend the system");
            return;
        }
        m_suspendCallback();
    });
}

bool CommandRouter::isAdapterValid() const {
    return m_adapter.isConnected();
}

bool CommandRouter::isSuspended() const {
    std::lock_guard<std::mutex> lock(m_routerMutex);
    return m_suspended;
}

} // namespace cec_control
