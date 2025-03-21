#include "cec_daemon.h"
#include "../common/logger.h"
#include "../common/config_manager.h"

#include <csignal>
#include <cstdlib>
#include <unistd.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <signal.h>
#include <unordered_set>
#include <future>
#include <atomic>

namespace cec_control {

// Static instance pointer for signal handler
CECDaemon* CECDaemon::s_instance = nullptr;
// Track how many times we've received termination signals
static std::atomic<int> s_termSignalCount{0};

CECDaemon::CECDaemon(Options options) 
    : m_running(false), 
      m_suspended(false), 
      m_options(options),
      m_queueCommandsDuringSuspend(options.queueCommandsDuringSuspend) {
    
    // Create thread pool for background tasks (4 threads)
    m_threadPool = std::make_shared<ThreadPool>(4);
    
    // Set static instance
    s_instance = this;
}

CECDaemon::~CECDaemon() {
    stop();
    s_instance = nullptr;
}

bool CECDaemon::start() {
    // Add mutex to protect initialization
    static std::mutex startupMutex;
    std::lock_guard<std::mutex> lock(startupMutex);
    
    // Start thread pool
    m_threadPool->start();
    
    LOG_INFO("Starting CEC daemon");
    
    try {
        // Create CEC manager with our options and shared thread pool
        CECManager::Options cecOptions;
        cecOptions.scanDevicesAtStartup = m_options.scanDevicesAtStartup;
        
        m_cecManager = std::make_unique<CECManager>(cecOptions, m_threadPool);
        
        // Initialize CEC manager
        if (!m_cecManager->initialize()) {
            LOG_ERROR("Failed to initialize CEC manager");
            
            // If running under systemd, exit with proper code
            if (getenv("NOTIFY_SOCKET") != nullptr) {
                LOG_INFO("Notifying systemd of failure");
                return false;
            } else {
                LOG_INFO("Exiting daemon due to no CEC adapters found");
                exit(EXIT_FAILURE);
            }
        }
        
        // Create socket server with shared thread pool
        LOG_INFO("Creating socket server with shared thread pool");
        m_socketServer = std::make_unique<SocketServer>(m_threadPool);
        m_socketServer->setCommandHandler([this](const Message& cmd) {
            return this->handleCommand(cmd);
        });
        
        LOG_INFO("Starting socket server");
        if (!m_socketServer->start()) {
            LOG_ERROR("Failed to start socket server");
            m_cecManager->shutdown();
            return false;
        }
        
        // Set up signal handlers
        LOG_INFO("Setting up signal handlers");
        setupSignalHandlers();
        
        // Set up power monitoring if enabled
        if (m_options.enablePowerMonitor) {
            LOG_INFO("Setting up power monitor");
            if (!setupPowerMonitor()) {
                LOG_WARNING("Failed to set up power monitoring. Sleep/wake events will not be handled automatically.");
            }
        } else {
            LOG_INFO("D-Bus power monitoring disabled via configuration. Suspend/resume operations will require manual commands.");
        }
        
        m_running = true;
        LOG_INFO("CEC daemon started successfully");
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon startup: ", e.what());
        return false;
    }
}

void CECDaemon::stop() {
    // Add mutex to protect shutdown
    static std::mutex shutdownMutex;
    std::lock_guard<std::mutex> lock(shutdownMutex);
    
    if (!m_running) return;
    
    LOG_INFO("Stopping CEC daemon");
    
    // Set running flag to false to exit main loop
    m_running = false;
    
    // Clear any queued commands
    {
        std::lock_guard<std::mutex> lock(m_queuedCommandsMutex);
        if (!m_queuedCommands.empty()) {
            LOG_INFO("Clearing ", m_queuedCommands.size(), " queued commands on shutdown");
            m_queuedCommands.clear();
        }
    }
    
    // If running under systemd, log shutdown
    if (getenv("NOTIFY_SOCKET") != nullptr) {
        LOG_INFO("Stopping daemon under systemd control");
    }
    
    try {
        // Stop D-Bus monitor
        if (m_dbusMonitor) {
            LOG_INFO("Stopping D-Bus monitor");
            m_dbusMonitor->stop();
        }
        
        // Shutdown socket server
        if (m_socketServer) {
            LOG_INFO("Stopping socket server");
            auto serverStopStart = std::chrono::steady_clock::now();
            m_socketServer->stop();
            auto serverStopDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - serverStopStart).count();
            LOG_INFO("Socket server stopped in ", serverStopDuration, "ms");
        }
        
        // Shutdown CEC manager
        if (m_cecManager) {
            LOG_INFO("Shutting down CEC manager");
            auto cecShutdownStart = std::chrono::steady_clock::now();
            m_cecManager->shutdown();
            auto cecShutdownDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - cecShutdownStart).count();
            LOG_INFO("CEC manager shutdown completed in ", cecShutdownDuration, "ms");
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon shutdown: ", e.what());
    }
    
    // Safely release resources
    LOG_INFO("Releasing resources");
    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_cecManager.reset();
    
    // Shutdown thread pool last since other components might use it during their shutdown
    if (m_threadPool) {
        LOG_INFO("Shutting down thread pool");
        m_threadPool->shutdown();
        m_threadPool.reset();
    }
    
    LOG_INFO("CEC daemon stopped - shutdown sequence complete");
}

void CECDaemon::run() {
    LOG_INFO("Entering main daemon loop");
    
    std::mutex loopMutex;
    std::unique_lock<std::mutex> loopLock(loopMutex);
    std::condition_variable loopCondition;
    
    while (m_running) {
        // Only check connection if not suspended
        if (!m_suspended.load(std::memory_order_acquire) && 
            m_cecManager && !m_cecManager->isAdapterValid()) {
            
            LOG_WARNING("CEC connection lost, attempting to reconnect");
            
            if (m_cecManager->reconnect()) {
                LOG_INFO("Successfully reconnected to CEC adapter");
            } else {
                LOG_ERROR("Failed to reconnect to CEC adapter - will retry");
            }
        }
        
        // Use timed wait instead of sleep to allow immediate exit
        loopCondition.wait_for(loopLock, std::chrono::seconds(1), [this] {
            return !m_running;  // Check running state periodically
        });
    }
}

void CECDaemon::onSuspend() {
    // Protect the suspended state transition
    std::lock_guard<std::mutex> lock(m_suspendMutex);
    
    if (m_suspended) return;
    
    LOG_INFO("System suspending, preparing CEC adapter");
    m_suspended = true;
    
    // Safety timeout using thread pool
    m_threadPool->submit([this]() {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        if (m_suspended.load(std::memory_order_acquire) && m_dbusMonitor) {
            LOG_WARNING("Safety timeout reached - releasing inhibitor lock forcibly");
            m_dbusMonitor->releaseInhibitLock();
        }
    });
    
    try {
        // Only release CEC adapter resources, keep socket server running
        if (m_cecManager) {
            auto startTime = std::chrono::steady_clock::now();
            
            // Send standby command to connected devices before shutting down
            LOG_INFO("Sending standby commands to connected CEC devices");
            m_cecManager->standbyDevices();
            
            m_cecManager->shutdown();
            LOG_INFO("CEC adapter suspended");
            
            // Calculate how long the operation took
            auto shutdownDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            LOG_INFO("CEC shutdown took ", shutdownDuration, "ms");
            
            // Release the inhibitor lock to allow the system to proceed with sleep
            if (m_dbusMonitor) {
                LOG_INFO("CEC sleep preparation complete, allowing system to sleep");
                m_dbusMonitor->releaseInhibitLock();
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during suspend: ", e.what());
        
        // Make sure we release the lock even in case of error
        if (m_dbusMonitor) {
            m_dbusMonitor->releaseInhibitLock();
        }
    }
}

void CECDaemon::onResume() {
    // Protect the suspended state transition
    std::lock_guard<std::mutex> lock(m_suspendMutex);
    
    if (!m_suspended) return;
    
    LOG_INFO("System resuming, reinitializing CEC adapter");
    
    try {
        // When resuming, simply request the CEC manager to reconnect
        bool reconnectSuccessful = m_cecManager && m_cecManager->reconnect();
        
        if (reconnectSuccessful) {
            LOG_INFO("CEC adapter reconnected successfully on resume");
            
            // Power on connected CEC devices
            LOG_INFO("Powering on connected CEC devices");
            m_cecManager->powerOnDevices();
        } else {
            LOG_ERROR("Failed to reconnect CEC adapter on resume");
            
            // Schedule a background reconnection attempt
            m_threadPool->submit([this]() {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                
                // Check if we should still attempt to reconnect
                if (!m_suspended.load(std::memory_order_acquire) && 
                    m_cecManager && 
                    !m_cecManager->isAdapterValid()) {
                    
                    LOG_INFO("Performing delayed reconnection attempt");
                    m_cecManager->reconnect();
                }
            });
        }
        
        // Mark as not suspended regardless of reconnection result
        m_suspended = false;
        
        // Process queued commands if reconnection was successful
        if (m_queueCommandsDuringSuspend && reconnectSuccessful) {
            std::vector<Message> queuedCommands;
            
            // Get queued commands and clear the queue atomically
            {
                std::lock_guard<std::mutex> lock(m_queuedCommandsMutex);
                if (m_queuedCommands.empty()) {
                    return;
                }
                
                queuedCommands = std::move(m_queuedCommands);
                m_queuedCommands.clear();
            }
            
            // Process each queued command
            LOG_INFO("Processing ", queuedCommands.size(), " queued commands");
            for (const auto& cmd : queuedCommands) {
                try {
                    m_cecManager->processCommand(cmd);
                }
                catch (const std::exception& e) {
                    LOG_ERROR("Exception processing queued command: ", e.what());
                }
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during resume: ", e.what());
        m_suspended = false;  // Ensure we exit suspended state even on error
    }
}

void CECDaemon::processSuspendCommand() {
    LOG_INFO("Processing suspend command");
    onSuspend();
}

void CECDaemon::processResumeCommand() {
    LOG_INFO("Processing resume command");
    onResume();
}

bool CECDaemon::setupPowerMonitor() {
    LOG_INFO("Setting up D-Bus power monitoring");
    
    try {
        // Create D-Bus monitor
        m_dbusMonitor = std::make_unique<DBusMonitor>();
        
        // Initialize D-Bus connection
        if (!m_dbusMonitor->initialize()) {
            LOG_ERROR("Failed to initialize D-Bus monitor");
            return false;
        }
        
        // Start monitoring power events
        m_dbusMonitor->start([this](DBusMonitor::PowerState state) {
            this->handlePowerStateChange(state);
        });
        
        LOG_INFO("D-Bus power monitoring setup successfully");
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during D-Bus power monitor setup: ", e.what());
        return false;
    }
}

void CECDaemon::handlePowerStateChange(DBusMonitor::PowerState state) {
    switch (state) {
        case DBusMonitor::PowerState::Suspending:
            LOG_INFO("Received system suspend notification from D-Bus");
            onSuspend();
            break;
            
        case DBusMonitor::PowerState::Resuming:
            LOG_INFO("Received system resume notification from D-Bus");
            onResume();
            break;
    }
}

Message CECDaemon::handleCommand(const Message& command) {
    LOG_DEBUG("Received command: type=", static_cast<int>(command.type), 
              ", deviceId=", static_cast<int>(command.deviceId));
    
    // Handle suspend/resume commands
    if (command.type == MessageType::CMD_SUSPEND) {
        processSuspendCommand();
        return Message(MessageType::RESP_SUCCESS);
    } else if (command.type == MessageType::CMD_RESUME) {
        processResumeCommand();
        return Message(MessageType::RESP_SUCCESS);
    }
    
    if (!m_cecManager) {
        LOG_ERROR("CEC manager not initialized");
        return Message(MessageType::RESP_ERROR);
    }
    
    // Check if system is suspended
    if (m_suspended.load(std::memory_order_acquire)) {
        // Commands that make sense to queue during suspend
        static const std::unordered_set<MessageType> queueableCommands = {
            MessageType::CMD_POWER_ON,
            MessageType::CMD_POWER_OFF,
            MessageType::CMD_VOLUME_UP,
            MessageType::CMD_VOLUME_DOWN,
            MessageType::CMD_VOLUME_MUTE
        };
        
        // Queue commands if enabled and command type is queueable
        if (m_queueCommandsDuringSuspend && queueableCommands.count(command.type) > 0) {
            std::lock_guard<std::mutex> lock(m_queuedCommandsMutex);
            m_queuedCommands.push_back(command);
            LOG_INFO("Queued command type=", static_cast<int>(command.type), 
                     " for execution after resume");
            return Message(MessageType::RESP_SUCCESS);
        }
        
        // Command can't be queued
        LOG_WARNING("Command received while suspended and can't be queued");
        return Message(MessageType::RESP_ERROR);
    }
    
    // Use the command queue inside CECManager
    return m_cecManager->processCommand(command);
}

void CECDaemon::setupSignalHandlers() {
    // Set up signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGHUP, signalHandler);
}

void CECDaemon::signalHandler(int signal) {
    // Signal handlers should be minimal to reduce race conditions
    static std::atomic<bool> shutdownInitiated{false};
    
    try {
        LOG_INFO("Received signal ", signal);
        
        // Get the singleton instance
        CECDaemon* instance = CECDaemon::getInstance();
        if (!instance) {
            _exit(1);
            return;
        }
        
        // Set running to false to break the main loop
        instance->m_running = false;
        
        // For termination signals (SIGTERM, SIGINT)
        if (signal == SIGTERM || signal == SIGINT) {
            int count = ++s_termSignalCount;
            
            // First signal: initiate graceful shutdown
            if (count == 1 && !shutdownInitiated.exchange(true)) {
                LOG_INFO("Initiating graceful shutdown sequence");
                
                // Use thread pool for shutdown
                if (instance->m_threadPool) {
                    instance->m_threadPool->submit([instance]() {
                        LOG_INFO("Shutdown thread started");
                        
                        try {
                            // Stop all daemon components in order
                            instance->stop();
                            LOG_INFO("Shutdown completed successfully");
                        } 
                        catch (const std::exception& e) {
                            LOG_ERROR("Exception during shutdown: ", e.what());
                        }
                        
                        // Exit after shutdown thread completes
                        _exit(0);
                    });
                } else {
                    // Fallback if thread pool is unavailable
                    std::thread([instance]() {
                        LOG_INFO("Shutdown thread started (fallback mode)");
                        try {
                            instance->stop();
                            LOG_INFO("Shutdown completed successfully");
                        } catch (...) {
                            LOG_ERROR("Exception during shutdown");
                        }
                        _exit(0);
                    }).detach();
                }
                
                return;
            } 
            // Multiple signals: force immediate exit
            else if (count > 1) {
                LOG_INFO("Exiting immediately due to multiple signals (", count, ")");
                _exit(0);
                return;
            }
        } 
        // Other signals like SIGHUP
        else {
            // For other signals, initiate normal shutdown
            LOG_INFO("Starting normal shutdown for signal ", signal);
            instance->stop();
        }
    }
    catch (const std::exception& e) {
        try {
            LOG_ERROR("Exception in signal handler: ", e.what());
        } catch (...) {}
        _exit(1);
    }
    catch (...) {
        _exit(1);
    }
}

} // namespace cec_control
