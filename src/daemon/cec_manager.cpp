#include "cec_manager.h"
#include "../common/logger.h"

#include <csignal>
#include <thread>

namespace cec_control {

CECManager::CECManager(Options options) 
    : m_options(options) {
    // Create the components
    CECAdapter::Options adapterOptions;
    adapterOptions.deviceName = "CEC Controller";
    
    CommandThrottler::Options throttlerOptions;
    throttlerOptions.baseIntervalMs = 200;
    throttlerOptions.maxIntervalMs = 1000;
    throttlerOptions.maxRetryAttempts = 3;
    
    m_adapter = std::make_shared<CECAdapter>(adapterOptions);
    m_throttler = std::make_shared<CommandThrottler>(throttlerOptions);
    m_deviceOps = std::make_shared<DeviceOperations>(m_adapter, m_throttler);
    
    // Create command queue
    m_commandQueue = std::make_unique<CommandQueue>();
    m_commandQueue->setOperationHandler([this](const Message& cmd) {
        return this->handleCommand(cmd);
    });
}

CECManager::~CECManager() {
    // Stop command queue first to prevent new operations
    if (m_commandQueue) {
        m_commandQueue->stop();
    }
    
    shutdown();
}

bool CECManager::initialize() {
    LOG_INFO("Initializing CEC manager");
    
    // Initialize the adapter
    if (!m_adapter->initialize()) {
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
    LOG_INFO("Shutting down CEC manager");
    
    // Stop the command queue to prevent new operations
    if (m_commandQueue) {
        m_commandQueue->stop();
    }
    
    // Shutdown the adapter
    if (m_adapter) {
        m_adapter->shutdown();
    }
}

bool CECManager::reconnect() {
    LOG_INFO("Attempting to reconnect to CEC adapter");
    
    // Make sure we're disconnected first but don't log extra messages
    if (m_commandQueue) {
        m_commandQueue->stop();
    }
    
    if (m_adapter) {
        m_adapter->shutdown();
    }
    
    // Wait a moment before reconnecting
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Try to reconnect, tracking failures
    static int reconnectFailures = 0;
    bool result = initialize();
    
    if (!result) {
        reconnectFailures++;
        LOG_ERROR("Reconnect attempt failed (", reconnectFailures, " consecutive failures)");
        
        // After multiple consecutive failures, trigger shutdown
        if (reconnectFailures >= 3) {
            LOG_ERROR("Multiple reconnect failures - daemon will exit");
            
            // If running as a systemd service, we want to exit with an error
            if (getenv("NOTIFY_SOCKET") != nullptr) {
                LOG_INFO("Notifying systemd of persistent adapter failure");
                exit(EXIT_FAILURE);
            } else {
                // Schedule shutdown signal in a separate thread to avoid deadlocks
                std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    LOG_FATAL("Exiting due to persistent CEC adapter failure");
                    exit(EXIT_FAILURE);
                }).detach();
            }
        }
    } else {
        // Reset failure counter on successful reconnect
        reconnectFailures = 0;
    }
    
    return result;
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
    if (timeoutMs == 0) {
        timeoutMs = m_options.commandTimeoutMs;
    }
    
    // Determine priority based on command type
    CECOperation::Priority priority = CECOperation::Priority::NORMAL;
    if (command.type == MessageType::CMD_RESTART_ADAPTER) {
        priority = CECOperation::Priority::HIGH;
    }
    
    return m_commandQueue->enqueue(command, priority, timeoutMs);
}

Message CECManager::handleCommand(const Message& command) {
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
            
        case MessageType::CMD_RESTART_ADAPTER:
            // Handle restart command asynchronously to avoid deadlock
            std::thread([this]() {
                LOG_INFO("Performing asynchronous adapter restart");
                this->shutdown();
                
                // Wait a moment before reconnecting
                std::this_thread::sleep_for(std::chrono::seconds(1));
                
                // Try to reconnect
                this->initialize();
                LOG_INFO("Asynchronous adapter restart completed");
            }).detach();
            
            // Return success immediately, actual restart happens in background
            success = true;
            break;
            
        default:
            LOG_ERROR("Unknown command type: ", static_cast<int>(command.type));
            return Message(MessageType::RESP_ERROR);
    }
    
    if (success) {
        return Message(MessageType::RESP_SUCCESS);
    } else {
        return Message(MessageType::RESP_ERROR);
    }
}

void CECManager::scanDevices() {
    if (!isAdapterValid()) return;
    
    m_deviceOps->scanDevices();
}

} // namespace cec_control
