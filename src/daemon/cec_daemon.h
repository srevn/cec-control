#pragma once

#include <atomic>
#include <memory>
#include <signal.h>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "cec_manager.h"
#include "socket_server.h"
#include "../common/logger.h"

namespace cec_control {

class CECDaemon {
public:
    // Configuration options for the daemon
    struct Options {
        bool scanDevicesAtStartup;       // Whether to scan for devices at startup
        bool queueCommandsDuringSuspend; // Whether to queue commands during suspend
        
        Options() 
            : scanDevicesAtStartup(false),
              queueCommandsDuringSuspend(true) {}  // Queue commands by default
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
    std::atomic<bool> m_running;
    std::atomic<bool> m_suspended;
    Options m_options;
    
    static CECDaemon* s_instance;
    
    // Power management
    std::thread m_powerMonitorThread;
    std::mutex m_powerMonitorMutex;
    std::condition_variable m_powerMonitorCV;
    std::atomic<bool> m_powerMonitorRunning;
    
    // Command queuing during suspend
    std::mutex m_queuedCommandsMutex;
    std::vector<Message> m_queuedCommands;
    bool m_queueCommandsDuringSuspend;
    
    // Command handler for socket messages
    Message handleCommand(const Message& command);
    
    // Setup signal handlers
    void setupSignalHandlers();
    
    // Power monitoring thread function
    void monitorPowerEvents();
    
    // Helper to monitor system sleep events via logind
    bool setupPowerMonitor();
    
    // Cleanup power monitor resources
    void cleanupPowerMonitor();
    
    // Helper to notify system we're ready for sleep via D-Bus
    void notifyReadyForSleep();
};

} // namespace cec_control