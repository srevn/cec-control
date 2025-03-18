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
#include "../common/logger.h"

namespace cec_control {

class CECDaemon {
public:
    // Configuration options for the daemon
    struct Options {
        bool scanDevicesAtStartup;       // Whether to scan for devices at startup
        bool queueCommandsDuringSuspend; // Whether to queue commands during suspend
        bool enablePowerMonitor;         // Whether to enable D-Bus power state monitoring
        
        Options() 
            : scanDevicesAtStartup(false),
              queueCommandsDuringSuspend(true),
              enablePowerMonitor(true) {}
    };
    
    CECDaemon(Options options = Options());
    ~CECDaemon();
    
    // Initialize and start daemon
    bool start();
    
    // Shutdown daemon gracefully
    void stop();
    
    // Run the daemon main loop
    void run();
    
    // Handle signals (static to be used with signal())
    static void signalHandler(int signal);
    
    // Get singleton instance 
    static CECDaemon* getInstance() { return s_instance; }
    
    // System suspend/resume handling
    void onSuspend();
    void onResume();
    
    // Manual suspend/resume commands
    void processSuspendCommand();
    void processResumeCommand();

private:
    std::unique_ptr<CECManager> m_cecManager;
    std::unique_ptr<SocketServer> m_socketServer;
    std::unique_ptr<DBusMonitor> m_dbusMonitor;
    std::atomic<bool> m_running;
    std::atomic<bool> m_suspended;  // Keep as atomic for quick checks
    std::mutex m_suspendMutex;      // Add mutex for suspend/resume operations
    Options m_options;
    
    static CECDaemon* s_instance;
    
    // Command queuing during suspend
    std::mutex m_queuedCommandsMutex;
    std::vector<Message> m_queuedCommands;
    bool m_queueCommandsDuringSuspend;
    
    // Command handler for socket messages
    Message handleCommand(const Message& command);
    
    // Setup signal handlers
    void setupSignalHandlers();
    
    // D-Bus power monitoring
    void handlePowerStateChange(DBusMonitor::PowerState state);
    
    // Setup power monitoring
    bool setupPowerMonitor();
};

} // namespace cec_control