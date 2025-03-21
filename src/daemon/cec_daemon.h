#pragma once

#include <atomic>
#include <memory>
#include <signal.h>
#include <thread>
#include <mutex>
#include <condition_variable>

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
    // Core components
    std::unique_ptr<CECManager> m_cecManager;
    std::unique_ptr<SocketServer> m_socketServer;
    std::unique_ptr<DBusMonitor> m_dbusMonitor;
    std::shared_ptr<ThreadPool> m_threadPool;
    
    // State flags
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_suspended{false};
    
    // Synchronization
    std::mutex m_suspendMutex;
    
    // Configuration
    Options m_options;
    
    // Singleton instance
    static CECDaemon* s_instance;
    
    // Command queuing during suspend
    std::mutex m_queuedCommandsMutex;
    std::vector<Message> m_queuedCommands;
    bool m_queueCommandsDuringSuspend;
    
    /**
     * @brief Command handler for socket messages
     * @param command Command to handle
     * @return Response message
     */
    Message handleCommand(const Message& command);
    
    /**
     * @brief Setup signal handlers
     */
    void setupSignalHandlers();
    
    /**
     * @brief Handle power state change from D-Bus
     * @param state New power state
     */
    void handlePowerStateChange(DBusMonitor::PowerState state);
    
    /**
     * @brief Setup power monitoring
     * @return true if setup successful
     */
    bool setupPowerMonitor();
};

} // namespace cec_control