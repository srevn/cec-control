#include "cec_manager.h"
#include "../common/logger.h"
#include "../common/config_manager.h"

#include <csignal>
#include <thread>
#include <mutex>

namespace cec_control {

// Mutex for synchronizing adapter operations across all instances
static std::mutex g_adapterMutex;

CECManager::CECManager(Options options) 
    : m_options(options) {
    // Create the components
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
                if (deviceId >= 0 && deviceId <= 15) { // Valid CEC logical addresses are 0-15
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
                if (deviceId >= 0 && deviceId <= 15) { // Valid CEC logical addresses are 0-15
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
    std::lock_guard<std::mutex> lock(g_adapterMutex);
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
    std::lock_guard<std::mutex> lock(g_adapterMutex);
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
    // Take the global adapter mutex to ensure only one thread
    // manipulates the adapter at a time
    std::lock_guard<std::mutex> lock(g_adapterMutex);
    
    LOG_INFO("Attempting to reconnect to CEC adapter");
    
    // Track reconnect failures with a static counter
    static int reconnectFailures = 0;
    
    // Check if adapter is already connected - quick exit
    if (isAdapterValid()) {
        LOG_INFO("Adapter already connected, no need to reconnect");
        reconnectFailures = 0;
        return true;
    }
    
    try {
        // Check if we need to shut down first - only if adapter is initialized but not connected
        bool needsShutdown = m_adapter && m_adapter->hasAdapter() && !m_adapter->isConnected();
        
        if (needsShutdown) {
            LOG_DEBUG("Shutting down adapter before reconnection attempt");
            m_adapter->shutdown();
            
            // Wait before reconnecting when we had to shut down first
            LOG_DEBUG("Brief pause before reinitializing CEC adapter");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            LOG_DEBUG("Adapter already shut down, proceeding to initialization");
        }
        
        // Try to initialize the adapter
        if (!m_adapter->initialize()) {
            LOG_ERROR("Failed to initialize CEC adapter during reconnect attempt");
            reconnectFailures++;
            
            if (reconnectFailures >= 3) {
                LOG_ERROR("Multiple reconnect failures (", reconnectFailures, ") - daemon will exit");
                
                // If running as a systemd service, exit with error
                if (getenv("NOTIFY_SOCKET") != nullptr) {
                    LOG_INFO("Notifying systemd of persistent adapter failure");
                    std::thread([]() {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        LOG_FATAL("Exiting due to persistent CEC adapter failure");
                        exit(EXIT_FAILURE);
                    }).detach();
                }
            }
            
            return false;
        }
        
        // Reset failure counter on successful adapter initialization
        reconnectFailures = 0;
        
        // Start the command queue if needed
        if (!m_commandQueue->isRunning()) {
            LOG_INFO("Starting command queue");
            if (!m_commandQueue->start()) {
                LOG_ERROR("Failed to start command queue during reconnect");
                m_adapter->shutdown();
                return false;
            }
        } else {
            LOG_DEBUG("Command queue is already running");
        }
        
        LOG_INFO("CEC adapter reconnected successfully");
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during reconnect: ", e.what());
        return false;
    }
    catch (...) {
        LOG_ERROR("Unknown exception during reconnect");
        return false;
    }
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
    // Take a read lock on the adapter mutex to check validity
    std::lock_guard<std::mutex> lock(g_adapterMutex);
    
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
            if (!command.data.empty()) {
                bool enabled = command.data[0] > 0;
                if (m_adapter) {
                    m_adapter->setAutoStandby(enabled);
                    success = true;
                }
            }
            break;
        
        case MessageType::CMD_RESTART_ADAPTER:
            // Create a shared future for the restart operation
            {
                LOG_INFO("Processing restart adapter command");
                
                // Run this in a new thread but use the reconnect method which has proper synchronization
                std::thread([this]() {
                    LOG_INFO("Performing asynchronous adapter restart");
                    
                    // Acquire the adapter mutex so no other operations can interfere
                    std::lock_guard<std::mutex> lock(g_adapterMutex);
                    
                    // First shut down the adapter
                    this->m_adapter->shutdown();
                    
                    // A brief pause to let everything settle
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    
                    // Now reinitialize - don't use reconnect() here to avoid deadlock
                    // with the g_adapterMutex we're already holding
                    bool success = this->m_adapter->initialize();
                    
                    if (success) {
                        // Start command queue if needed
                        if (!this->m_commandQueue->isRunning()) {
                            this->m_commandQueue->start();
                        }
                        LOG_INFO("Adapter restart completed successfully");
                    } else {
                        LOG_ERROR("Failed to restart adapter");
                    }
                }).detach();
                
                // Return success immediately, actual restart happens in background
                success = true;
            }
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

bool CECManager::standbyDevices() {
    // Take the global adapter mutex to ensure thread safety
    std::lock_guard<std::mutex> lock(g_adapterMutex);
    
    if (!m_adapter || !isAdapterValid()) {
        LOG_ERROR("Cannot standby devices - CEC adapter not initialized or not connected");
        return false;
    }
    
    try {
        LOG_INFO("Sending standby commands to configured devices");
        // Use CECDEVICE_BROADCAST to use the powerOffDevices bitmask from configuration
        return m_adapter->standbyDevices(CEC::CECDEVICE_BROADCAST);
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception sending standby commands: ", e.what());
        return false;
    }
}

bool CECManager::powerOnDevices() {
    // Take the global adapter mutex to ensure thread safety
    std::lock_guard<std::mutex> lock(g_adapterMutex);
    
    if (!m_adapter || !isAdapterValid()) {
        LOG_ERROR("Cannot power on devices - CEC adapter not initialized or not connected");
        return false;
    }
    
    try {
        LOG_INFO("Sending power on commands to configured devices");
        // According to the API, this will use the wakeDevices bitmask from configuration
        // when using CECDEVICE_TV (which is the default)
        return m_adapter->powerOnDevices(CEC::CECDEVICE_TV);
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception sending power on commands: ", e.what());
        return false;
    }
}

void CECManager::scanDevices() {
    if (!isAdapterValid()) return;
    
    m_deviceOps->scanDevices();
}

} // namespace cec_control
