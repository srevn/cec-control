#pragma once

#include <thread>
#include <atomic>
#include <functional>
#include <systemd/sd-bus.h>

namespace cec_control {

/**
 * Class to monitor power management events via D-Bus using sd-bus.
 * Simplified implementation with robust error handling and connection management.
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
     * Starts the monitoring thread to watch for power management signals.
     * 
     * @param callback Function to call when power state changes occur
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
    sd_bus* m_bus;                     // sd-bus connection
    sd_bus_slot* m_signalSlot;         // Signal subscription slot
    int m_inhibitFd;                   // File descriptor for inhibit lock
    
    std::thread m_thread;              // Monitoring thread
    std::atomic<bool> m_running;       // Thread running flag
    int m_shutdownPipe[2];             // Pipe for shutdown signaling
    PowerStateCallback m_callback;     // Power state callback
    
    /**
     * @brief Main monitoring loop - handles sd-bus event processing
     */
    void eventLoop();
    
    /**
     * @brief Static callback for PrepareForSleep signal
     * 
     * @param msg D-Bus message containing the signal
     * @param userdata User data pointer (this object)
     * @param ret_error Error return parameter
     * @return Signal processing result
     */
    static int onPrepareForSleep(sd_bus_message* msg, void* userdata, sd_bus_error* ret_error);
    
    /**
     * @brief Helper to format sd-bus error messages
     * 
     * @param error sd-bus error code
     * @return Human-readable error string
     */
    const char* busErrorToString(int error);
};

} // namespace cec_control