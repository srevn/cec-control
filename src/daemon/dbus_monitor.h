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
     * @brief Power state events that can occur
     */
    enum class PowerState {
        Suspending,  // System is going to sleep
        Resuming     // System is waking up
    };
    
    /**
     * @brief Callback type for power state changes
     */
    using PowerStateCallback = std::function<void(PowerState)>;
    
    /**
     * @brief Constructor - initializes internal state
     */
    DBusMonitor();
    
    /**
     * @brief Destructor - ensures proper cleanup of D-Bus resources
     * 
     * Releases inhibitor locks and cleans up D-Bus connection
     */
    ~DBusMonitor();
    
    /**
     * @brief Initialize the D-Bus connection
     * 
     * Sets up the connection to the system bus, registers signal handlers,
     * and takes an initial inhibitor lock.
     * 
     * @return true if successfully initialized, false otherwise
     */
    bool initialize();
    
    /**
     * @brief Start monitoring for power state changes
     * 
     * Initializes epoll and starts the monitoring thread to watch for
     * power management signals.
     * 
     * @param callback Function to call when power state changes occur
     * @return true if monitoring started successfully, false otherwise
     */
    void start(PowerStateCallback callback);
    
    /**
     * @brief Stop monitoring
     * 
     * Terminates the monitoring thread and cleans up resources
     */
    void stop();
    
    /**
     * @brief Take an inhibitor lock to delay system sleep
     * 
     * This requests a delay-type inhibitor lock from systemd's logind,
     * which allows the application to perform cleanup before sleep.
     * 
     * @return true if the lock was successfully taken
     */
    bool takeInhibitLock();
    
    /**
     * @brief Release the inhibitor lock to allow system sleep
     * 
     * This should be called after completing sleep preparation to
     * allow the system to continue with the sleep process.
     * 
     * @return true if successfully released
     */
    bool releaseInhibitLock();

private:
    DBusConnection* m_connection;  // D-Bus connection
    
    std::thread m_thread;          // Monitoring thread
    std::atomic<bool> m_running;   // Thread running flag
    PowerStateCallback m_callback; // Power state callback
    int m_inhibitFd;               // File descriptor for inhibit lock
    
    /**
     * @brief Information about a D-Bus watch
     */
    struct WatchInfo {
        DBusWatch* watch;    // D-Bus watch object
        int fd;              // File descriptor to monitor
        unsigned int flags;  // Watch flags (readable, writable, etc.)
        bool enabled;        // Whether this watch is currently enabled
    };
    
    std::mutex m_watchMutex;
    std::vector<WatchInfo> m_watches;
    std::unordered_map<int, uint32_t> m_fdToPoller;
    
    // D-Bus timeout management
    struct TimeoutInfo {
        DBusTimeout* timeout;
        int interval;
        bool enabled;
        int64_t expiry;
    };
    
    std::mutex m_timeoutMutex;
    std::vector<TimeoutInfo> m_timeouts;
    
    /**
     * @brief Main monitoring loop - handles epoll events and D-Bus dispatching
     */
    void monitorLoop();
    
    /**
     * @brief Process a D-Bus message and trigger appropriate callbacks
     * @param msg D-Bus message to process
     */
    void processMessage(DBusMessage* msg);
    
    /**
     * @brief Update timeout expiry timestamp
     * @param info Timeout info to update
     */
    void updateTimeoutExpiry(TimeoutInfo& info);
    
    // Static callbacks for D-Bus watch functions
    static dbus_bool_t addWatchCallback(DBusWatch* watch, void* data);
    static void removeWatchCallback(DBusWatch* watch, void* data);
    static void toggleWatchCallback(DBusWatch* watch, void* data);
    
    // Static callbacks for D-Bus timeout functions
    static dbus_bool_t addTimeoutCallback(DBusTimeout* timeout, void* data);
    static void removeTimeoutCallback(DBusTimeout* timeout, void* data);
    static void toggleTimeoutCallback(DBusTimeout* timeout, void* data);
    
    /**
     * @brief Static message filter callback for D-Bus
     * 
     * This function is called by D-Bus for each incoming message that
     * matches our registered filters.
     */
    static DBusHandlerResult messageFilterCallback(DBusConnection *connection, 
                                                  DBusMessage *message, 
                                                  void *user_data);
};

} // namespace cec_control