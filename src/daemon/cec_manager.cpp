#include "cec_manager.h"
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

        if (auto* raw = adapter.getRawAdapter(); raw && raw->SetStreamPath(physicalAddress)) {
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

CECManager::CECManager(Options options, std::shared_ptr<ThreadPool> threadPool)
    : m_adapter(options.adapter),
      m_throttler(options.throttler),
      m_options(std::move(options)),
      m_threadPool(std::move(threadPool)) {

    m_adapter.setOnTvStandbyCallback([this]() {
        LOG_INFO("TV standby callback triggered. Initiating system suspend.");
        auto suspendTask = [this]() {
            if (m_suspendCallback) {
                LOG_INFO("Executing suspend command via callback");
                if (!m_suspendCallback()) {
                    LOG_ERROR("Failed to execute suspend command via callback");
                }
            } else {
                LOG_WARNING("No suspend callback configured, cannot suspend system");
            }
        };

        if (m_threadPool) {
            m_threadPool->submit(suspendTask);
        } else {
            std::thread(suspendTask).detach();
        }
    });
}

CECManager::~CECManager() {
    shutdown();
}

void CECManager::setConnectionLostCallback(std::function<void()> callback) {
    m_adapter.setConnectionLostCallback(std::move(callback));
}

void CECManager::setSuspendCallback(std::function<bool()> callback) {
    m_suspendCallback = std::move(callback);
}

void CECManager::setFatalErrorCallback(std::function<void()> callback) {
    m_fatalErrorCallback = std::move(callback);
}

bool CECManager::initialize() {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    LOG_INFO("Initializing CEC manager");

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
        scanDevices();
    } else {
        LOG_INFO("Skipping device scanning");
    }

    LOG_INFO("CEC manager initialized successfully");
    return true;
}

void CECManager::shutdown() {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    LOG_INFO("Shutting down CEC manager");
    m_adapter.closeConnection();
}

bool CECManager::reconnect() {
    std::lock_guard<std::mutex> lock(m_managerMutex);

    LOG_INFO("Attempting to reconnect to CEC adapter");

    if (isAdapterValid()) {
        LOG_INFO("Adapter already connected, no need to reconnect");
        m_reconnectFailures = 0;
        return true;
    }

    if (m_adapter.reopenConnection()) {
        m_reconnectFailures = 0;
        LOG_INFO("CEC adapter reconnected successfully");
        return true;
    }

    ++m_reconnectFailures;
    LOG_ERROR("Failed to reconnect CEC adapter (attempt ", m_reconnectFailures, ")");
    if (m_reconnectFailures >= 3) {
        LOG_ERROR("Multiple reconnect failures, signalling daemon shutdown");
        // Hand control back to the daemon's run loop, which terminates
        // cleanly. The supervising service (systemd) is responsible for
        // restarting us — no need to call exit() ourselves.
        if (m_fatalErrorCallback) {
            m_fatalErrorCallback();
        }
    }
    return false;
}

bool CECManager::isAdapterValid() const {
    return m_adapter.isConnected();
}

Message CECManager::processCommand(const Message& command) {
    std::lock_guard<std::mutex> lock(m_managerMutex);

    if (!isAdapterValid() && command.type != MessageType::CMD_RESTART_ADAPTER) {
        LOG_ERROR("Cannot process command: CEC adapter not connected");
        return Message(MessageType::RESP_ERROR);
    }

    bool success = false;
    switch (command.type) {
        case MessageType::CMD_VOLUME_UP:
            success = setVolume(m_adapter, m_throttler, command.deviceId, true);
            break;
        case MessageType::CMD_VOLUME_DOWN:
            success = setVolume(m_adapter, m_throttler, command.deviceId, false);
            break;
        case MessageType::CMD_VOLUME_MUTE:
            success = setMute(m_adapter, m_throttler, command.deviceId, true);
            break;
        case MessageType::CMD_POWER_ON:
            success = powerOnDevice(m_adapter, m_throttler, command.deviceId);
            break;
        case MessageType::CMD_POWER_OFF:
            success = powerOffDevice(m_adapter, m_throttler, command.deviceId);
            break;
        case MessageType::CMD_CHANGE_SOURCE:
            if (!command.data.empty()) {
                success = setSource(m_adapter, m_throttler, command.deviceId, command.data[0]);
            }
            break;
        case MessageType::CMD_AUTO_STANDBY:
            if (!command.data.empty()) {
                m_adapter.setAutoStandby(command.data[0] > 0);
                success = true;
            }
            break;
        case MessageType::CMD_RESTART_ADAPTER: {
            LOG_INFO("Scheduling adapter restart");

            // RESP_SUCCESS here means "accepted"; the actual reconnect runs
            // on a pool worker and logs its own result. Acquiring the lock
            // on the worker avoids re-entering it on the dispatching thread.
            auto restartTask = [this]() {
                std::lock_guard<std::mutex> restartLock(m_managerMutex);
                if (m_adapter.reopenConnection()) {
                    LOG_INFO("Adapter restart completed successfully");
                } else {
                    LOG_ERROR("Failed to restart adapter");
                }
            };

            if (m_threadPool) {
                m_threadPool->submit(restartTask);
            } else {
                std::thread(restartTask).detach();
            }
            success = true;
            break;
        }
        default:
            LOG_ERROR("Unknown command type: ", static_cast<int>(command.type));
            return Message(MessageType::RESP_ERROR);
    }

    return success ? Message(MessageType::RESP_SUCCESS) : Message(MessageType::RESP_ERROR);
}

bool CECManager::standbyDevices() {
    std::lock_guard<std::mutex> lock(m_managerMutex);

    if (!isAdapterValid()) {
        LOG_ERROR("Cannot standby devices - CEC adapter not initialized or not connected");
        return false;
    }

    try {
        LOG_INFO("Sending standby commands to configured devices");
        return m_adapter.standbyDevices(CEC::CECDEVICE_BROADCAST);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception sending standby commands: ", e.what());
        return false;
    }
}

bool CECManager::powerOnDevices() {
    std::lock_guard<std::mutex> lock(m_managerMutex);

    if (!isAdapterValid()) {
        LOG_ERROR("Cannot power on devices - CEC adapter not initialized or not connected");
        return false;
    }

    try {
        LOG_INFO("Sending power on commands to configured devices");
        return m_adapter.powerOnDevices(CEC::CECDEVICE_BROADCAST);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception sending power on commands: ", e.what());
        return false;
    }
}

void CECManager::scanDevices() {
    if (!isAdapterValid()) return;
    logDeviceSnapshot(m_adapter);
}

} // namespace cec_control
