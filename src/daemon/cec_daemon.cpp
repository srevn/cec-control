#include "cec_daemon.h"
#include "../common/logger.h"

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
static volatile int s_termSignalCount = 0;

CECDaemon::CECDaemon(Options options) 
    : m_running(false), 
      m_suspended(false), 
      m_options(options),
      m_queueCommandsDuringSuspend(options.queueCommandsDuringSuspend) {
    // Set static instance
    s_instance = this;
}

CECDaemon::~CECDaemon() {
    stop();
    s_instance = nullptr;
}

bool CECDaemon::start() {
    LOG_INFO("Starting CEC daemon");
    
    try {
        // Create CEC manager with our options
        CECManager::Options cecOptions;
        cecOptions.scanDevicesAtStartup = m_options.scanDevicesAtStartup;
        
        m_cecManager = std::make_unique<CECManager>(cecOptions);
        if (!m_cecManager) {
            LOG_ERROR("Failed to create CEC manager");
            return false;
        }
        
        // Initialize CEC manager
        if (!m_cecManager->initialize()) {
            LOG_ERROR("Failed to initialize CEC manager");
            
            // If no adapters found, exit with proper code
            LOG_FATAL("Failed to start CEC daemon - no CEC adapters found");
            
            // Set up exit via daemon manager if possible
            if (getenv("NOTIFY_SOCKET") != nullptr) {
                LOG_INFO("Notifying systemd of failure");
                // Return false will cause main to exit with error status
            } else {
                LOG_INFO("Exiting daemon due to no CEC adapters found");
                // Exit directly with error code
                exit(EXIT_FAILURE);
            }
            
            return false;
        }
        
        // Create socket server
        LOG_INFO("Creating socket server...");
        m_socketServer = std::make_unique<SocketServer>();
        if (!m_socketServer) {
            LOG_ERROR("Failed to create socket server");
            m_cecManager->shutdown();
            return false;
        }
        
        m_socketServer->setCommandHandler([this](const Message& cmd) {
            return this->handleCommand(cmd);
        });
        
        LOG_INFO("Starting socket server...");
        if (!m_socketServer->start()) {
            LOG_ERROR("Failed to start socket server");
            m_cecManager->shutdown();
            return false;
        }
        
        // Set up signal handlers
        LOG_INFO("Setting up signal handlers...");
        setupSignalHandlers();
        
        // Set up power monitoring
        LOG_INFO("Setting up power monitor...");
        if (!setupPowerMonitor()) {
            LOG_WARNING("Failed to set up power monitoring. Sleep/wake events will not be handled automatically.");
        }
        
        m_running = true;
        LOG_INFO("CEC daemon started successfully");
        
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon startup: ", e.what());
        return false;
    }
    catch (...) {
        LOG_ERROR("Unknown exception during daemon startup");
        return false;
    }
}

void CECDaemon::stop() {
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
    
    // Stop D-Bus monitor
    LOG_INFO("Stopping D-Bus monitor");
    if (m_dbusMonitor) {
        m_dbusMonitor->stop();
    }
    
    try {
        // Shutdown components in reverse order with timeouts
        if (m_socketServer) {
            LOG_INFO("Stopping socket server");
            auto serverStopStart = std::chrono::steady_clock::now();
            m_socketServer->stop();
            auto serverStopDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - serverStopStart).count();
            LOG_INFO("Socket server stopped in ", serverStopDuration, "ms");
        } else {
            LOG_INFO("Socket server already null, skipping stop");
        }
        
        if (m_cecManager) {
            LOG_INFO("Shutting down CEC manager");
            auto cecShutdownStart = std::chrono::steady_clock::now();
            m_cecManager->shutdown();
            auto cecShutdownDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - cecShutdownStart).count();
            LOG_INFO("CEC manager shutdown completed in ", cecShutdownDuration, "ms");
        } else {
            LOG_INFO("CEC manager already null, skipping shutdown");
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon shutdown: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception during daemon shutdown");
    }
    
    // Safely release resources
    LOG_INFO("Releasing resources");
    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_cecManager.reset();
    
    LOG_INFO("CEC daemon stopped - shutdown sequence complete");
}

void CECDaemon::run() {
    LOG_INFO("Entering main daemon loop");
    
    while (m_running) {
        // Only check connection if not suspended
        if (!m_suspended && m_cecManager && !m_cecManager->isAdapterValid()) {
            LOG_WARNING("CEC connection lost, attempting to reconnect");
            bool reconnected = m_cecManager->reconnect();
            
            if (!reconnected) {
                LOG_ERROR("Failed to reconnect to CEC adapter - will retry");
            } else {
                LOG_INFO("Successfully reconnected to CEC adapter");
            }
        }
        
        // Sleep to avoid busy loop
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void CECDaemon::onSuspend() {
    if (m_suspended) return;
    
    LOG_INFO("System suspending, preparing CEC adapter");
    m_suspended = true;
    
    try {
        // Only release CEC adapter resources, keep socket server running
        if (m_cecManager) {
            auto startTime = std::chrono::steady_clock::now();
            
            m_cecManager->shutdown();
            LOG_INFO("CEC adapter suspended");
            
            // Calculate how long the operation took
            auto shutdownDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime).count();
            LOG_INFO("CEC shutdown took ", shutdownDuration, "ms");
            
            // Now that preparation is complete, release the inhibitor lock
            // to allow the system to proceed with sleep
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
    catch (...) {
        LOG_ERROR("Unknown exception during suspend");
        
        // Make sure we release the lock even in case of error
        if (m_dbusMonitor) {
            m_dbusMonitor->releaseInhibitLock();
        }
    }
}

void CECDaemon::onResume() {
    if (!m_suspended) return;
    
    LOG_INFO("System resuming, reinitializing CEC adapter");
    bool reconnectSuccessful = false;
    
    try {
        // Reinitialize CEC adapter if needed
        if (m_cecManager) {
            reconnectSuccessful = m_cecManager->reconnect();
            if (!reconnectSuccessful) {
                LOG_WARNING("Failed to reconnect CEC adapter on resume, will retry later");
            } else {
                LOG_INFO("CEC adapter resumed");
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during resume: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception during resume");
    }
    
    m_suspended = false;
    
    // Process any queued commands if reconnection was successful
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
                if (m_cecManager) {
                    m_cecManager->processCommand(cmd);
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("Exception processing queued command: ", e.what());
            }
            catch (...) {
                LOG_ERROR("Unknown exception processing queued command");
            }
        }
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
    
    // Handle suspended state
    if (m_suspended && command.type != MessageType::CMD_RESUME) {
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
            {
                std::lock_guard<std::mutex> lock(m_queuedCommandsMutex);
                m_queuedCommands.push_back(command);
                LOG_INFO("Queued command type=", static_cast<int>(command.type), 
                         " for execution after resume");
            }
            return Message(MessageType::RESP_SUCCESS);
        }
        
        // Command can't be queued
        LOG_WARNING("Command received while suspended and can't be queued");
        return Message(MessageType::RESP_ERROR);
    }
    
    // Use the command queue inside CECManager now
    return m_cecManager->processCommand(command);
}

void CECDaemon::setupSignalHandlers() {
    // Set up signal handlers for graceful shutdown
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGHUP, signalHandler);
}

void CECDaemon::signalHandler(int signal) {
    LOG_INFO("Received signal ", signal);
    
    // Use the proper getter method to get the instance safely
    CECDaemon* instance = CECDaemon::getInstance();
    
    if (instance) {
        // Set running to false first to break the main loop
        instance->m_running = false;
        
        // For SIGTERM or SIGINT, track how many signals we've received
        if (signal == SIGTERM || signal == SIGINT) {
            // Use atomic operation to safely increment signal count
            static std::atomic<int> atomicTermSignalCount(0);
            int count = ++atomicTermSignalCount;
            s_termSignalCount = count; // For backwards compatibility
            
            if (count == 1) {
                // Create a thread with the instance pointer obtained via the getter
                std::thread shutdownThread([]() {
                    LOG_INFO("Shutdown initiated, please wait...");
                    
                    // Get the instance again inside the thread for safety
                    CECDaemon* instanceInThread = CECDaemon::getInstance();
                    if (instanceInThread) {
                        // Log each step of the shutdown process
                        LOG_INFO("Stopping daemon services");
                        instanceInThread->stop();
                        
                        // Signal successful exit
                        LOG_INFO("Shutdown completed successfully");
                        _exit(0); // Ensure we exit cleanly after shutdown
                    } else {
                        LOG_ERROR("Instance became null during shutdown thread execution");
                        _exit(1); // Error exit
                    }
                });
                
                // Detach the thread to let it run independently
                shutdownThread.detach();
                
                // Don't exit - allow the shutdown thread to handle everything
                return;
            } else {
                LOG_INFO("Exiting immediately due to multiple signals");
                _exit(0);
            }
        } else {
            // For other signals, just initiate normal shutdown
            instance->stop();
        }
    } else {
        LOG_ERROR("Signal handler called but instance is null");
    }
}

} // namespace cec_control
