#include "cec_manager.h"
#include "../common/logger.h"

#include <thread>
#include <utility>

namespace cec_control {

CECManager::CECManager(Options options, std::shared_ptr<ThreadPool> threadPool)
    : m_options(std::move(options)), m_threadPool(std::move(threadPool)) {

    m_adapter = std::make_shared<CECAdapter>(m_options.adapter);
    m_throttler = std::make_shared<CommandThrottler>(m_options.throttler);
    m_deviceOps = std::make_shared<DeviceOperations>(m_adapter, m_throttler);

    m_adapter->setOnTvStandbyCallback([this]() {
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
    if (m_adapter) {
        m_adapter->setConnectionLostCallback(std::move(callback));
    }
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

    if (!m_adapter->initialize()) {
        LOG_ERROR("Failed to initialize CEC adapter library");
        return false;
    }

    if (!m_adapter->openConnection()) {
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

    if (m_adapter) {
        m_adapter->closeConnection();
    }
}

bool CECManager::reconnect() {
    std::lock_guard<std::mutex> lock(m_managerMutex);

    LOG_INFO("Attempting to reconnect to CEC adapter");

    if (isAdapterValid()) {
        LOG_INFO("Adapter already connected, no need to reconnect");
        m_reconnectFailures = 0;
        return true;
    }

    if (m_adapter && m_adapter->reopenConnection()) {
        m_reconnectFailures = 0;
        LOG_INFO("CEC adapter reconnected successfully");
        return true;
    }

    m_reconnectFailures++;
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
    return m_adapter && m_adapter->isConnected();
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
            success = m_deviceOps->setVolume(command.deviceId, true);
            break;
        case MessageType::CMD_VOLUME_DOWN:
            success = m_deviceOps->setVolume(command.deviceId, false);
            break;
        case MessageType::CMD_VOLUME_MUTE:
            success = m_deviceOps->setMute(command.deviceId, true);
            break;
        case MessageType::CMD_POWER_ON:
            success = m_deviceOps->powerOnDevice(command.deviceId);
            break;
        case MessageType::CMD_POWER_OFF:
            success = m_deviceOps->powerOffDevice(command.deviceId);
            break;
        case MessageType::CMD_CHANGE_SOURCE:
            if (!command.data.empty()) {
                success = m_deviceOps->setSource(command.deviceId, command.data[0]);
            }
            break;
        case MessageType::CMD_AUTO_STANDBY:
            if (!command.data.empty() && m_adapter) {
                m_adapter->setAutoStandby(command.data[0] > 0);
                success = true;
            }
            break;
        case MessageType::CMD_RESTART_ADAPTER: {
            LOG_INFO("Scheduling adapter restart");

            // The restart runs async so the client call returns promptly.
            // RESP_SUCCESS here means "accepted"; the actual reconnect
            // happens on a pool worker and logs its own result.
            auto restartTask = [this]() {
                std::lock_guard<std::mutex> restartLock(m_managerMutex);
                if (m_adapter && m_adapter->reopenConnection()) {
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
        return m_adapter->standbyDevices(CEC::CECDEVICE_BROADCAST);
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
        return m_adapter->powerOnDevices(CEC::CECDEVICE_BROADCAST);
    } catch (const std::exception& e) {
        LOG_ERROR("Exception sending power on commands: ", e.what());
        return false;
    }
}

void CECManager::scanDevices() {
    if (!isAdapterValid()) return;

    m_deviceOps->scanDevices();
}

}  // namespace cec_control
