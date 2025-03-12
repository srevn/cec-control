#pragma once

#include <thread>
#include <atomic>
#include <functional>

// Forward declarations for D-Bus types
struct DBusConnection;
struct DBusMessage;

namespace cec_control {

/**
 * Class to monitor power management events via D-Bus.
 * Replaces the previous popen-based approach with proper D-Bus integration.
 */
class DBusMonitor {
public:
    /**
     * Power state events that can occur
     */
    enum class PowerState {
        Suspending,  // System is going to sleep
        Resuming     // System is waking up
    };
    
    /**
     * Callback type for power state changes
     */
    using PowerStateCallback = std::function<void(PowerState)>;
    
    /**
     * Constructor
     */
    DBusMonitor();
    
    /**
     * Destructor
     */
    ~DBusMonitor();
    
    /**
     * Initialize the D-Bus connection
     * @return true if successful, false otherwise
     */
    bool initialize();
    
    /**
     * Start monitoring for power state changes
     * @param callback Function to call when power state changes
     */
    void start(PowerStateCallback callback);
    
    /**
     * Stop monitoring
     */
    void stop();
    
    /**
     * Take an inhibitor lock to delay system sleep
     * @return true if the lock was successfully taken
     */
    bool takeInhibitLock();
    
    /**
     * Release the inhibitor lock to allow system sleep
     * @return true if successful
     */
    bool releaseInhibitLock();

private:
    DBusConnection* m_connection;  // D-Bus connection
    
    std::thread m_thread;         // Monitoring thread
    std::atomic<bool> m_running;  // Thread running flag
    PowerStateCallback m_callback; // Power state callback
    int m_inhibitFd;              // File descriptor for inhibit lock
    
    // Main monitoring loop
    void monitorLoop();
    
    // Process a D-Bus message
    void processMessage(DBusMessage* msg);
};

} // namespace cec_control
