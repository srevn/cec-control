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
    // Single-threaded context: the daemon calls initialize() before any
    // event source is attached, so no dispatch or lifecycle event can
    // race us. The state mutex is deliberately not taken.
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
    // Flip the shutdown flag and take ownership of the queue under the
    // state lock; everything that touches the adapter runs outside.
    std::vector<Message> toDiscard;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_shutdownComplete.load(std::memory_order_relaxed)) return;
        m_shutdownComplete.store(true, std::memory_order_release);
        toDiscard = std::move(m_queuedCommands);
        m_queuedCommands.clear();
    }

    LOG_INFO("Shutting down CEC command router");
    m_adapter.closeConnection();
    if (!toDiscard.empty()) {
        LOG_INFO("Discarding ", toDiscard.size(),
                 " queued commands on shutdown");
    }
}

bool CommandRouter::reconnect() {
    // State check under the lock, adapter work outside. Between the
    // check and the reopen, a DBus suspend or an outer stop() may land;
    // the resulting reopen-then-close pair is harmless (the adapter
    // mutex serialises them) and was already accepted as the cost of
    // not holding a multi-second reopen under a router lock.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_shutdownComplete.load(std::memory_order_relaxed)) {
            LOG_DEBUG("reconnect() called after shutdown; ignoring");
            return false;
        }
        if (m_suspended.load(std::memory_order_relaxed)) {
            // The adapter is intentionally closed during suspend; libCEC
            // won't see a connection-lost event here, so this branch
            // mostly fires when a stray alert lands between suspend()
            // and the kernel actually suspending. No-op and wait for
            // resume() to reopen.
            LOG_DEBUG("reconnect() called while suspended; ignoring");
            return false;
        }
    }

    if (m_adapter.isConnected()) {
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
    // Phase 1: flip the flag under m_stateMutex so any dispatch picking
    // up the lock after us sees the suspended state. New arrivals will
    // enter handleSuspendedLocked (queue or reject); in-flight phase-2
    // dispatches that already passed their phase-1 check complete
    // against the adapter concurrently with our closeConnection below.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_shutdownComplete.load(std::memory_order_relaxed)) {
            // shutdown() already closed the adapter and cleared the
            // queue; re-running the suspend sequence would only churn
            // flags.
            LOG_DEBUG("suspend() called after shutdown; ignoring");
            return;
        }
        if (m_suspended.load(std::memory_order_relaxed)) {
            LOG_DEBUG("suspend() called while already suspended");
            return;
        }
        m_suspended.store(true, std::memory_order_release);
    }

    // Phase 2: drive libcec outside the state lock. closeConnection
    // takes the adapter mutex internally and therefore waits briefly
    // (ms-granularity) for an in-flight libcec call to finish, but
    // does NOT wait on a throttler retry — those run lock-free now.
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
    // Phase 1: verify we are actually in the suspended state. Keep
    // m_suspended true across the reopen below so new dispatches
    // continue to queue rather than race the half-open adapter.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_shutdownComplete.load(std::memory_order_relaxed)) {
            // resume() would reopen an adapter shutdown() just destroyed.
            LOG_DEBUG("resume() called after shutdown; ignoring");
            return;
        }
        if (!m_suspended.load(std::memory_order_relaxed)) {
            LOG_DEBUG("resume() called while not suspended");
            return;
        }
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

    // Flip the flag and take ownership of any commands that queued
    // during the reopen. After the release store below, new dispatches
    // will fall through to phase 2 and may interleave with our drain.
    std::vector<Message> drained;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        drained = std::move(m_queuedCommands);
        m_queuedCommands.clear();
        m_suspended.store(false, std::memory_order_release);
    }

    if (!reconnected) {
        // Queued commands already got RESP_SUCCESS when accepted
        // ("accepted for post-resume"). We cannot deliver on that
        // promise, so log and drop.
        if (!drained.empty()) {
            LOG_WARNING("Discarding ", drained.size(),
                        " queued commands: reconnect failed");
        }
        return;
    }

    if (drained.empty()) return;

    LOG_INFO("Processing ", drained.size(), " queued commands");
    for (const auto& cmd : drained) {
        try {
            (void)executeCommand(cmd);
        } catch (const std::exception& e) {
            LOG_ERROR("Exception processing queued command: ", e.what());
        }
    }
}

Message CommandRouter::dispatch(const Message& command) {
    // Phase 1: state decisions under m_stateMutex. Brief — no libcec
    // calls, no sleeps, no pool submits beyond scheduleRestart's
    // fire-and-forget.
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        if (m_shutdownComplete.load(std::memory_order_relaxed)) {
            return Message(MessageType::RESP_ERROR);
        }
        if (command.type == MessageType::CMD_RESTART_ADAPTER) {
            return handleRestartLocked(command);
        }
        if (m_suspended.load(std::memory_order_relaxed)) {
            return handleSuspendedLocked(command);
        }
    }

    // Phase 2: execute with no router lock held. The adapter mutex
    // serialises libcec calls; the throttler serialises its own state
    // via atomics. A concurrent suspend() between the state check and
    // here will race us on the adapter mutex — we either complete our
    // call first (adapter still open) or find the adapter closed and
    // fall through to the RESP_ERROR path.
    if (!m_adapter.isConnected()) {
        LOG_ERROR("Cannot process command: CEC adapter not connected");
        return Message(MessageType::RESP_ERROR);
    }
    return executeCommand(command);
}

Message CommandRouter::handleRestartLocked(const Message& /*command*/) {
    // Shutdown was ruled out by the outer dispatch() check; we only
    // need to guard against the suspended case. Reopening mid-suspend
    // races libcec against USB suspend and must be refused.
    if (m_suspended.load(std::memory_order_relaxed)) {
        LOG_WARNING("CMD_RESTART_ADAPTER rejected: router is suspended");
        return Message(MessageType::RESP_ERROR);
    }
    LOG_INFO("Scheduling adapter restart");
    scheduleRestart();
    return Message(MessageType::RESP_SUCCESS);
}

Message CommandRouter::handleSuspendedLocked(const Message& command) {
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

Message CommandRouter::executeCommand(const Message& command) {
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
        // State check under m_stateMutex, reopen outside. A concurrent
        // suspend/shutdown that lands between the check and the reopen
        // is tolerated — see scheduleRestart doc-comment in the header.
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            const bool shuttingDown =
                m_shutdownComplete.load(std::memory_order_relaxed);
            const bool suspended =
                m_suspended.load(std::memory_order_relaxed);
            if (shuttingDown || suspended) {
                LOG_INFO("Adapter restart skipped: router is ",
                         shuttingDown ? "shut down" : "suspended");
                return;
            }
        }
        if (m_adapter.reopenConnection()) {
            LOG_INFO("Adapter restart completed successfully");
        } else {
            LOG_ERROR("Failed to restart adapter");
        }
    });
}

void CommandRouter::onTvStandby() {
    // Runs on libCEC's internal thread. Must not touch m_stateMutex —
    // the mutex is brief but taking it from a libcec callback would
    // still risk subtle ordering hazards with the dispatch path.
    if (!m_autoStandbyEnabled.load(std::memory_order_acquire)) {
        LOG_DEBUG("TV standby observed; auto-standby disabled — ignoring");
        return;
    }

    if (!m_suspendCallback) {
        LOG_WARNING("TV standby observed but no suspend callback wired");
        return;
    }

    LOG_INFO("TV standby observed with auto-standby enabled; initiating system suspend");
    // The callback's contract (see Callbacks doc) is "thread-safe and
    // non-blocking" — in practice a single MainThreadWork::post. Running
    // it inline here avoids a thread-pool hop that added only scheduling
    // latency, and removes one more lambda that captures this.
    m_suspendCallback();
}

bool CommandRouter::isAdapterValid() const {
    return m_adapter.isConnected();
}

bool CommandRouter::isSuspended() const {
    return m_suspended.load(std::memory_order_acquire);
}

} // namespace cec_control
