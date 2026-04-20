#include "cec_daemon.h"
#include "../common/logger.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <future>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <utility>

#include "../common/event_poller.h"

namespace cec_control {

CECDaemon* CECDaemon::s_instance = nullptr;

CECDaemon::CECDaemon(Options daemonOptions, CECManager::Options managerOptions)
    : m_options(daemonOptions),
      m_managerOptions(std::move(managerOptions)),
      m_queueCommandsDuringSuspend(m_options.queueCommandsDuringSuspend) {

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
    static std::mutex startupMutex;
    std::lock_guard<std::mutex> lock(startupMutex);

    LOG_INFO("Starting CEC daemon");

    // Wake fd must be created before installing signal handlers, since the
    // handler writes into it. Non-blocking + close-on-exec.
    m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeFd < 0) {
        LOG_ERROR("Failed to create wakeup eventfd: ", std::strerror(errno));
        return false;
    }

    m_threadPool->start();

    try {
        // Consume the prebuilt options into the manager. m_managerOptions is
        // not read again after this point.
        m_cecManager = std::make_unique<CECManager>(std::move(m_managerOptions), m_threadPool);
        m_cecManager->setConnectionLostCallback([this]() { this->onConnectionLost(); });
        m_cecManager->setSuspendCallback([this]() -> bool {
            return m_dbusMonitor ? m_dbusMonitor->suspendSystem() : false;
        });
        m_cecManager->setFatalErrorCallback([this]() {
            // Signal the run loop to exit; supervising service will restart us.
            m_running.store(false, std::memory_order_release);
            wakeMainLoop();
        });

        if (!m_cecManager->initialize()) {
            LOG_ERROR("Failed to initialize CEC manager");
            return false;
        }

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

        LOG_INFO("Setting up signal handlers");
        setupSignalHandlers();

        if (m_options.enablePowerMonitor) {
            LOG_INFO("Setting up power monitor");
            if (!setupPowerMonitor()) {
                LOG_WARNING("Failed to set up power monitoring. Sleep/wake events will not be handled automatically.");
            }
        } else {
            LOG_INFO("D-Bus power monitoring disabled via configuration. Suspend/resume operations will require manual commands.");
        }

        m_running.store(true, std::memory_order_release);
        LOG_INFO("CEC daemon started successfully");

        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon startup: ", e.what());
        return false;
    }
}

void CECDaemon::stop() {
    static std::mutex shutdownMutex;
    std::lock_guard<std::mutex> lock(shutdownMutex);

    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    LOG_INFO("Stopping CEC daemon");

    // Restore default signal handlers before any other teardown, so a signal
    // arriving mid-shutdown doesn't try to call into a half-destroyed daemon.
    teardownSignalHandlers();

    // Wake the run loop so it can exit on its next iteration. Safe even if
    // run() has already returned.
    wakeMainLoop();

    {
        std::lock_guard<std::mutex> qLock(m_queuedCommandsMutex);
        if (!m_queuedCommands.empty()) {
            LOG_INFO("Clearing ", m_queuedCommands.size(), " queued commands on shutdown");
            m_queuedCommands.clear();
        }
    }

    if (getenv("NOTIFY_SOCKET") != nullptr) {
        LOG_INFO("Stopping daemon under systemd control");
    }

    try {
        if (m_dbusMonitor) {
            LOG_INFO("Stopping D-Bus monitor");
            m_dbusMonitor->stop();
        }

        if (m_socketServer) {
            LOG_INFO("Stopping socket server");
            auto serverStopStart = std::chrono::steady_clock::now();
            m_socketServer->stop();
            auto serverStopDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - serverStopStart).count();
            LOG_INFO("Socket server stopped in ", serverStopDuration, "ms");
        }

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

    LOG_INFO("Releasing resources");
    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_cecManager.reset();

    if (m_threadPool) {
        LOG_INFO("Shutting down thread pool");
        m_threadPool->shutdown();
        m_threadPool.reset();
    }

    // Close wake fd after handlers are restored. Setting m_wakeFd to -1
    // before close ensures any concurrent (best-effort) signal handler
    // observes the invalid fd and skips its write().
    if (int fd = std::exchange(m_wakeFd, -1); fd >= 0) {
        ::close(fd);
    }

    LOG_INFO("CEC daemon stopped - shutdown sequence complete");
}

void CECDaemon::run() {
    LOG_INFO("Entering main daemon loop");

    if (m_wakeFd < 0) {
        LOG_ERROR("Wake fd not initialised; main loop cannot run");
        return;
    }

    EventPoller poller;
    if (!poller.add(m_wakeFd, static_cast<uint32_t>(EventPoller::Event::READ))) {
        LOG_ERROR("Failed to register wake fd with event poller");
        return;
    }

    while (m_running.load(std::memory_order_acquire)) {
        // Block until a wakeup arrives (signal handler, onConnectionLost,
        // or stop()). EINTR yields an empty event vector; nullopt means the
        // poller is unusable and we should exit so systemd can restart us.
        auto events = poller.wait(-1);
        if (!events) {
            LOG_ERROR("Event poller failed; main loop exiting");
            m_running.store(false, std::memory_order_release);
            break;
        }
        drainWakeFd();

        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }

        if (m_connectionLost.exchange(false, std::memory_order_acq_rel)) {
            onConnectionLostEvent();
        }
    }

    LOG_DEBUG("Main daemon loop exited");
}

void CECDaemon::onConnectionLost() {
    LOG_INFO("CEC connection lost event received.");
    m_connectionLost.store(true, std::memory_order_release);
    wakeMainLoop();
}

void CECDaemon::onConnectionLostEvent() {
    if (m_suspended.load(std::memory_order_acquire) || !m_cecManager) {
        return;
    }
    LOG_WARNING("CEC connection lost, attempting to reconnect");
    if (m_cecManager->reconnect()) {
        LOG_INFO("Successfully reconnected to CEC adapter");
    } else {
        LOG_ERROR("Failed to reconnect to CEC adapter - will retry on next event");
    }
}

void CECDaemon::wakeMainLoop() noexcept {
    int fd = m_wakeFd;
    if (fd < 0) return;
    const uint64_t one = 1;
    ssize_t r = ::write(fd, &one, sizeof(one));
    (void)r;  // Best-effort: if the eventfd is full or closed we have nothing useful to do.
}

void CECDaemon::drainWakeFd() noexcept {
    if (m_wakeFd < 0) return;
    uint64_t scratch;
    while (::read(m_wakeFd, &scratch, sizeof(scratch)) > 0) {
        // eventfd is a counter; one read returns the current sum and zeros it.
    }
}

void CECDaemon::onSuspend() {
    // Protect the suspended state transition
    std::lock_guard<std::mutex> lock(m_suspendMutex);
    
    if (m_suspended) return;
    
    LOG_INFO("System suspending, preparing CEC adapter");
    m_suspended = true;
    
    auto suspendCompletePromise = std::make_shared<std::promise<void>>();
    std::shared_future<void> suspendCompleteFuture = suspendCompletePromise->get_future();

    // Safety timeout using thread pool
    m_threadPool->submit([this, suspendCompleteFuture]() {
        if (suspendCompleteFuture.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
            if (m_suspended.load(std::memory_order_acquire) && m_dbusMonitor) {
                LOG_WARNING("Safety timeout reached - releasing inhibitor lock forcibly");
                m_dbusMonitor->releaseInhibitLock();
            }
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

    suspendCompletePromise->set_value();
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
    // sigaction is preferred over std::signal: well-defined semantics across
    // POSIX implementations, and we explicitly want signals to interrupt
    // blocking syscalls (no SA_RESTART) so the run loop wakes deterministically.
    struct sigaction sa{};
    sa.sa_handler = &CECDaemon::signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
}

void CECDaemon::teardownSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
}

void CECDaemon::signalHandler(int /*signum*/) {
    // Strictly async-signal-safe: only atomic stores, write() on an eventfd,
    // and _exit(). No logging, no condition variables, no allocator calls.
    CECDaemon* instance = s_instance;
    if (!instance) {
        _exit(EXIT_FAILURE);
    }

    instance->m_running.store(false, std::memory_order_release);

    int fd = instance->m_wakeFd;
    if (fd >= 0) {
        const uint64_t one = 1;
        ssize_t r = ::write(fd, &one, sizeof(one));
        (void)r;  // Best-effort wake; the loop will re-check m_running anyway.
    }

    // Escalate to forced exit on the third signal so a wedged shutdown can
    // still be aborted by a determined operator.
    if (instance->m_signalCount.fetch_add(1, std::memory_order_relaxed) >= 2) {
        _exit(EXIT_FAILURE);
    }
}

} // namespace cec_control
