#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <unordered_map>
#include <mutex>

#include <dbus/dbus.h>

namespace cec_control {

/**
 * Class to monitor power management events via D-Bus.
 * Uses asynchronous D-Bus API with proper event-driven approach.
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
    
    std::thread m_thread;          // Monitoring thread
    std::atomic<bool> m_running;   // Thread running flag
    PowerStateCallback m_callback; // Power state callback
    int m_inhibitFd;               // File descriptor for inhibit lock
    
    // D-Bus watch management
    struct WatchInfo {
        DBusWatch* watch;
        int fd;
        unsigned int flags;
        bool enabled;
    };
    
    std::mutex m_watchMutex;
    std::vector<WatchInfo> m_watches;
    
    // D-Bus timeout management
    struct TimeoutInfo {
        DBusTimeout* timeout;
        int interval;
        bool enabled;
        int64_t expiry;
    };
    
    std::mutex m_timeoutMutex;
    std::vector<TimeoutInfo> m_timeouts;
    
    // Main monitoring loop
    void monitorLoop();
    
    // Process a D-Bus message
    void processMessage(DBusMessage* msg);
    
    // Update timeout expiry
    void updateTimeoutExpiry(TimeoutInfo& info);
    
    // Static callbacks for D-Bus watch functions
    static dbus_bool_t addWatchCallback(DBusWatch* watch, void* data);
    static void removeWatchCallback(DBusWatch* watch, void* data);
    static void toggleWatchCallback(DBusWatch* watch, void* data);
    
    // Static callbacks for D-Bus timeout functions
    static dbus_bool_t addTimeoutCallback(DBusTimeout* timeout, void* data);
    static void removeTimeoutCallback(DBusTimeout* timeout, void* data);
    static void toggleTimeoutCallback(DBusTimeout* timeout, void* data);
    
    // Static message filter callback for D-Bus
    static DBusHandlerResult messageFilterCallback(DBusConnection *connection, 
                                                  DBusMessage *message, 
                                                  void *user_data);
};

} // namespace cec_control
