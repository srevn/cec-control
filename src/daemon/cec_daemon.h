#pragma once

#include <atomic>
#include <memory>
#include <signal.h>
#include <thread>
#include <mutex>

#include "cec_manager.h"
#include "socket_server.h"
#include "dbus_monitor.h"
#include "thread_pool.h"
#include "../common/logger.h"

namespace cec_control {

/**
 * @class CECDaemon
 * @brief Main daemon class that handles CEC control, socket communication, and system events
 * 
 * This class manages the lifecycle of the CEC daemon, including initialization, 
 * command handling, system power state monitoring, and graceful shutdown processes.
 * It acts as a central coordinator between the CEC manager, socket server, and D-Bus monitor.
 */
class CECDaemon {
public:
    /**
     * @brief Configuration options for the daemon
     */
    struct Options {
        bool scanDevicesAtStartup;       // Whether to scan for devices at startup
        bool queueCommandsDuringSuspend; // Whether to queue commands during suspend
        bool enablePowerMonitor;         // Whether to enable D-Bus power state monitoring
        
        Options() 
            : scanDevicesAtStartup(false),
              queueCommandsDuringSuspend(true),
              enablePowerMonitor(true) {}
    };
    
    /**
     * @brief Constructor
     * @param options Configuration options
     */
    explicit CECDaemon(Options options = Options());
    
    /**
     * @brief Destructor
     */
    ~CECDaemon();
    
    /**
     * @brief Initialize and start daemon
     * @return true if daemon started successfully
     */
    bool start();
    
    /**
     * @brief Shutdown daemon gracefully
     */
    void stop();
    
    /**
     * @brief Run the daemon main loop
     */
    void run();
    
    /**
     * @brief Handle signals (static to be used with signal())
     * @param signal Signal number
     */
    static void signalHandler(int signal);
    
    /**
     * @brief Get singleton instance
     * @return Pointer to daemon instance
     */
    static CECDaemon* getInstance() { return s_instance; }
    
    /**
     * @brief Handle system suspend event
     */
    void onSuspend();
    
    /**
     * @brief Handle system resume event
     */
    void onResume();
    
    /**
     * @brief Process explicit suspend command
     */
    void processSuspendCommand();
    
    /**
     * @brief Process explicit resume command
     */
    void processResumeCommand();

private:
    /**
     * @brief Callback for CEC adapter connection loss
     */
    void onConnectionLost();

    // Core components
    std::unique_ptr<CECManager> m_cecManager;
    std::unique_ptr<SocketServer> m_socketServer;
    std::unique_ptr<DBusMonitor> m_dbusMonitor;
    std::shared_ptr<ThreadPool> m_threadPool;
    
    // State flags
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_suspended{false};
    std::atomic<bool> m_connectionLost{false};

    // Counts how many termination signals have been received. The third one
    // escalates to _exit() in the signal handler.
    std::atomic<int> m_signalCount{0};

    // eventfd that the signal handler and onConnectionLost() write to in order
    // to wake the main loop. Created in start() before signal handlers are
    // installed; closed in stop() after handlers are restored to SIG_DFL.
    int m_wakeFd{-1};

    std::mutex m_suspendMutex;

    Options m_options;

    static CECDaemon* s_instance;

    // Command queuing during suspend
    std::mutex m_queuedCommandsMutex;
    std::vector<Message> m_queuedCommands;
    bool m_queueCommandsDuringSuspend;

    /** Command handler dispatched from socket messages. */
    Message handleCommand(const Message& command);

    /** Install SIGINT/SIGTERM/SIGHUP handlers via sigaction. */
    void setupSignalHandlers();

    /** Restore the affected signals to SIG_DFL. Idempotent. */
    void teardownSignalHandlers();

    /** Wake the main loop by writing to m_wakeFd. Async-signal-safe; best-effort. */
    void wakeMainLoop() noexcept;

    /** Drain any accumulated counter writes from m_wakeFd. */
    void drainWakeFd() noexcept;

    /** React to a connection-loss event surfaced by the adapter. */
    void onConnectionLostEvent();

    /** Handle power state change from D-Bus. */
    void handlePowerStateChange(DBusMonitor::PowerState state);

    /** Setup power monitoring. Returns true on success. */
    bool setupPowerMonitor();
};

} // namespace cec_control