#include "cec_manager.h"
#include "../common/logger.h"
#include "../common/config_manager.h"

#include <sstream>
#include <thread>
#include <mutex>

namespace cec_control {

CECManager::CECManager(Options options, std::shared_ptr<ThreadPool> threadPool)
    : m_options(options), m_threadPool(threadPool) {

    CECAdapter::Options adapterOptions;

    // Load adapter options from configuration
    auto& config = ConfigManager::getInstance();
    adapterOptions.deviceName = config.getString("Adapter", "DeviceName", "CEC Controller");
    adapterOptions.autoPowerOn = config.getBool("Adapter", "AutoPowerOn", false);
    adapterOptions.autoWakeAVR = config.getBool("Adapter", "AutoWakeAVR", false);
    adapterOptions.activateSource = config.getBool("Adapter", "ActivateSource", false);
    adapterOptions.systemAudioMode = config.getBool("Adapter", "SystemAudioMode", false);
    adapterOptions.powerOffOnStandby = config.getBool("Adapter", "PowerOffOnStandby", false);

    // Parse wake devices string (comma-separated list of logical addresses)
    std::string wakeDevicesStr = config.getString("Adapter", "WakeDevices", "");
    if (!wakeDevicesStr.empty()) {
        adapterOptions.wakeDevices.Clear();
        std::stringstream ss(wakeDevicesStr);
        std::string device;
        while (std::getline(ss, device, ',')) {
            try {
                int deviceId = std::stoi(device);
                if (deviceId >= 0 && deviceId <= 15) {  // Valid CEC logical addresses are 0-15
                    adapterOptions.wakeDevices.Set((CEC::cec_logical_address)deviceId);
                }
            } catch (const std::exception& e) {
                LOG_WARNING("Invalid wake device address in config: ", device);
            }
        }
    }

    // Parse power off devices string (comma-separated list of logical addresses)
    std::string powerOffDevicesStr = config.getString("Adapter", "PowerOffDevices", "");
    if (!powerOffDevicesStr.empty()) {
        adapterOptions.powerOffDevices.Clear();
        std::stringstream ss(powerOffDevicesStr);
        std::string device;
        while (std::getline(ss, device, ',')) {
            try {
                int deviceId = std::stoi(device);
                if (deviceId >= 0 && deviceId <= 15) {  // Valid CEC logical addresses are 0-15
                    adapterOptions.powerOffDevices.Set((CEC::cec_logical_address)deviceId);
                }
            } catch (const std::exception& e) {
                LOG_WARNING("Invalid power off device address in config: ", device);
            }
        }
    }

    // Create command throttler with proper options
    CommandThrottler::Options throttlerOptions;
    throttlerOptions.baseIntervalMs = config.getInt("Throttler", "BaseIntervalMs", 200);
    throttlerOptions.maxIntervalMs = config.getInt("Throttler", "MaxIntervalMs", 1000);
    throttlerOptions.maxRetryAttempts = config.getInt("Throttler", "MaxRetryAttempts", 3);

    m_adapter = std::make_shared<CECAdapter>(adapterOptions);
    m_throttler = std::make_shared<CommandThrottler>(throttlerOptions);
    m_deviceOps = std::make_shared<DeviceOperations>(m_adapter, m_throttler);

    // Set up the callback for TV standby events
    m_adapter->setOnTvStandbyCallback([this]() {
        LOG_INFO("TV standby callback triggered. Initiating system suspend.");
        auto suspendTask = [this]() {
            if (m_suspendCallback) {
                LOG_INFO("Executing suspend command via callback");
                bool success = m_suspendCallback();
                if (success) {
                    LOG_INFO("Suspend command executed successfully");
                } else {
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

    // Create command queue
    m_commandQueue = std::make_unique<CommandQueue>();
    m_commandQueue->setOperationHandler([this](const Message& cmd) { return this->handleCommand(cmd); });
}

CECManager::~CECManager() {
    // Stop command queue first to prevent new operations
    if (m_commandQueue) {
        m_commandQueue->stop();
    }

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

bool CECManager::initialize() {
    std::lock_guard<std::mutex> lock(m_managerMutex);
    LOG_INFO("Initializing CEC manager");

    // Initialize the adapter
    if (!m_adapter->openConnection()) {
        LOG_ERROR("Failed to initialize CEC adapter");
        return false;
    }

    // Start the command queue
    if (!m_commandQueue->start()) {
        LOG_ERROR("Failed to start command queue");
        shutdown();
        return false;
    }

    // Scan for CEC devices if the option is enabled
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

    // Stop the command queue to prevent new operations
    if (m_commandQueue) {
        m_commandQueue->stop();
    }

    // Shutdown the adapter
    if (m_adapter) {
        m_adapter->closeConnection();
    }
}

bool CECManager::reconnect() {
    // Global adapter mutex
    std::lock_guard<std::mutex> lock(m_managerMutex);

    LOG_INFO("Attempting to reconnect to CEC adapter");

    // Check if adapter is already connected - quick exit
    if (isAdapterValid()) {
        LOG_INFO("Adapter already connected, no need to reconnect");
        m_reconnectFailures = 0;
        return true;
    }

    if (m_adapter && m_adapter->reopenConnection()) {
        m_reconnectFailures = 0;
        LOG_INFO("CEC adapter reconnected successfully");

        // Start the command queue if needed
        if (!m_commandQueue->isRunning() && !m_commandQueue->start()) {
            LOG_ERROR("Failed to start command queue during reconnect");
            m_adapter->closeConnection();
            return false;
        }

        return true;
    }

    m_reconnectFailures++;
    LOG_ERROR("Failed to reconnect CEC adapter (attempt ", m_reconnectFailures, ")");
    if (m_reconnectFailures >= 3) {
        LOG_ERROR("Multiple reconnect failures, daemon will exit");
        // If running as a systemd service, schedule exit
        if (getenv("NOTIFY_SOCKET") != nullptr) {
            LOG_INFO("Notifying systemd of persistent adapter failure");

            auto exitFunc = []() {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                LOG_FATAL("Exiting due to persistent CEC adapter failure");
                exit(EXIT_FAILURE);
            };

            // Use thread pool if available, otherwise fall back to detached thread
            if (m_threadPool) {
                m_threadPool->submit(exitFunc);
            } else {
                std::thread(exitFunc).detach();
            }
        }
    }
    return false;
}

bool CECManager::isAdapterValid() const {
    return m_adapter && m_adapter->isConnected();
}

Message CECManager::processCommand(const Message& command) {
    // Synchronous version - delegates to command queue
    return m_commandQueue->executeSync(command, m_options.commandTimeoutMs);
}

std::shared_ptr<CECOperation> CECManager::processCommandAsync(const Message& command, uint32_t timeoutMs) {
    // Asynchronous version - returns operation that can be waited on
    uint32_t timeout = timeoutMs == 0 ? m_options.commandTimeoutMs : timeoutMs;

    // Determine priority based on command type
    CECOperation::Priority priority = (command.type == MessageType::CMD_RESTART_ADAPTER) ?
                                        CECOperation::Priority::HIGH :
                                        CECOperation::Priority::NORMAL;

    return m_commandQueue->enqueue(command, priority, timeout);
}

Message CECManager::handleCommand(const Message& command) {
    // Take a read lock on the adapter mutex to check validity
    std::lock_guard<std::mutex> lock(m_managerMutex);

    if (!isAdapterValid() && command.type != MessageType::CMD_RESTART_ADAPTER) {
        LOG_ERROR("Cannot process command: CEC adapter not connected");
        return Message(MessageType::RESP_ERROR);
    }

    bool success = false;

    // Delegate commands to device operations
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
            LOG_INFO("Processing restart adapter command");

            // Define the restart task
            auto restartTask = [this]() {
                LOG_INFO("Performing asynchronous adapter restart");

                // Acquire the adapter mutex so no other operations can interfere
                std::lock_guard<std::mutex> lock(m_managerMutex);

                bool success = this->m_adapter->reopenConnection();

                if (success) {
                    // Start command queue if needed
                    if (!this->m_commandQueue->isRunning()) {
                        this->m_commandQueue->start();
                    }
                    LOG_INFO("Adapter restart completed successfully");
                } else {
                    LOG_ERROR("Failed to restart adapter");
                }
            };

            // Use thread pool for better resource management
            if (m_threadPool) {
                m_threadPool->submit(restartTask);
            } else {
                std::thread(restartTask).detach();
            }

            // Return success immediately, actual restart happens in background
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
    // Take the global adapter mutex to ensure thread safety
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
    // Take the global adapter mutex to ensure thread safety
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
