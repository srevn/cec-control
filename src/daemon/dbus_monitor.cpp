#include "dbus_monitor.h"
#include "../common/logger.h"

#include <dbus/dbus.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>

namespace cec_control {

DBusMonitor::DBusMonitor() 
    : m_connection(nullptr),
      m_running(false),
      m_inhibitFd(-1) {
}

DBusMonitor::~DBusMonitor() {
    stop();
    
    // Make sure we release our inhibit lock
    if (m_inhibitFd >= 0) {
        releaseInhibitLock();
    }
    
    if (m_connection) {
        dbus_connection_unref(m_connection);
        m_connection = nullptr;
    }
}

bool DBusMonitor::initialize() {
    DBusError error;
    dbus_error_init(&error);
    
    // Connect to the system bus
    m_connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    
    if (dbus_error_is_set(&error)) {
        LOG_ERROR("Failed to connect to system D-Bus: ", error.message);
        dbus_error_free(&error);
        return false;
    }
    
    if (!m_connection) {
        LOG_ERROR("Failed to get D-Bus connection");
        return false;
    }
    
    // Add match rule for PrepareForSleep signal
    dbus_bus_add_match(m_connection, 
        "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'", 
        &error);
        
    if (dbus_error_is_set(&error)) {
        LOG_ERROR("Failed to add D-Bus match rule: ", error.message);
        dbus_error_free(&error);
        return false;
    }
    
    // Flush the connection to make sure match rules are applied
    dbus_connection_flush(m_connection);
    
    // Take an inhibitor lock to ensure we can delay sleep
    if (!takeInhibitLock()) {
        LOG_WARNING("Failed to take inhibitor lock - sleep delays may not work properly");
    }
    
    LOG_INFO("D-Bus monitor initialized successfully");
    return true;
}

void DBusMonitor::start(PowerStateCallback callback) {
    if (m_running) {
        LOG_WARNING("D-Bus monitor already running");
        return;
    }
    
    m_callback = callback;
    m_running = true;
    
    // Start monitoring thread
    m_thread = std::thread(&DBusMonitor::monitorLoop, this);
    
    LOG_INFO("D-Bus monitor started");
}

void DBusMonitor::stop() {
    if (!m_running) {
        return;
    }
    
    LOG_INFO("Stopping D-Bus monitor");
    m_running = false;
    
    // Wait for thread to exit
    if (m_thread.joinable()) {
        LOG_DEBUG("Waiting for D-Bus monitor thread to exit");
        m_thread.join();
    }
    
    LOG_INFO("D-Bus monitor stopped");
}

bool DBusMonitor::takeInhibitLock() {
    if (!m_connection) {
        LOG_ERROR("D-Bus connection not initialized");
        return false;
    }
    
    // If we already have an inhibit lock, don't get another one
    if (m_inhibitFd >= 0) {
        LOG_DEBUG("Already have an inhibit lock (fd=", m_inhibitFd, ")");
        return true;
    }
    
    LOG_INFO("Taking systemd inhibitor lock");
    
    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.login1",        // destination
        "/org/freedesktop/login1",       // path
        "org.freedesktop.login1.Manager", // interface
        "Inhibit");                      // method
        
    if (!msg) {
        LOG_ERROR("Failed to create D-Bus message for Inhibit");
        return false;
    }
    
    // Add arguments for the Inhibit call:
    // - what: "sleep" - we want to delay sleep
    // - who: our daemon name
    // - why: a user-visible reason
    // - mode: "delay" - we want to delay, not completely block
    const char* what = "sleep";
    const char* who = "cec-daemon";
    const char* why = "Preparing CEC adapter for sleep";
    const char* mode = "delay";
    
    if (!dbus_message_append_args(msg,
                               DBUS_TYPE_STRING, &what,
                               DBUS_TYPE_STRING, &who,
                               DBUS_TYPE_STRING, &why,
                               DBUS_TYPE_STRING, &mode,
                               DBUS_TYPE_INVALID)) {
        LOG_ERROR("Failed to append args to Inhibit message");
        dbus_message_unref(msg);
        return false;
    }
    
    // Send the message and wait for reply
    DBusError error;
    dbus_error_init(&error);
    
    LOG_DEBUG("Sending Inhibit message");
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        m_connection, msg, 5000, &error);
    
    dbus_message_unref(msg);
    
    if (dbus_error_is_set(&error)) {
        LOG_ERROR("Failed to get reply for Inhibit: ", error.message);
        dbus_error_free(&error);
        return false;
    }
    
    if (!reply) {
        LOG_ERROR("Got null reply for Inhibit");
        return false;
    }
    
    // Extract the file descriptor from the reply
    DBusError extractError;
    dbus_error_init(&extractError);
    
    int fd = -1;
    if (!dbus_message_get_args(reply, &extractError,
                             DBUS_TYPE_UNIX_FD, &fd,
                             DBUS_TYPE_INVALID)) {
        LOG_ERROR("Failed to extract fd from Inhibit reply: ", 
                 extractError.message);
        dbus_error_free(&extractError);
        dbus_message_unref(reply);
        return false;
    }
    
    dbus_message_unref(reply);
    
    // Store the file descriptor
    m_inhibitFd = fd;
    
    LOG_INFO("Successfully took inhibitor lock (fd=", m_inhibitFd, ")");
    return true;
}

bool DBusMonitor::releaseInhibitLock() {
    // Check if we have a lock to release
    if (m_inhibitFd < 0) {
        LOG_DEBUG("No inhibitor lock to release");
        return true;
    }
    
    LOG_INFO("Releasing inhibitor lock (fd=", m_inhibitFd, ")");
    
    // Close the file descriptor to release the lock
    close(m_inhibitFd);
    m_inhibitFd = -1;
    
    LOG_INFO("Inhibitor lock released");
    return true;
}

void DBusMonitor::monitorLoop() {
    if (!m_connection) {
        LOG_ERROR("D-Bus connection not initialized");
        return;
    }
    
    LOG_INFO("D-Bus monitor thread started");
    
    DBusMessage* msg = nullptr;
    
    // Set up the connection to not block the whole process
    dbus_connection_set_watch_functions(
        m_connection,
        nullptr, // add_watch
        nullptr, // remove_watch
        nullptr, // watch_toggled
        nullptr, // data
        nullptr  // free_data_function
    );
    
    while (m_running) {
        // Non-blocking dispatch
        dbus_connection_read_write_dispatch(m_connection, 0);
        
        // Check for new messages
        msg = dbus_connection_pop_message(m_connection);
        
        if (msg) {
            try {
                processMessage(msg);
            } catch (const std::exception& e) {
                LOG_ERROR("Exception processing D-Bus message: ", e.what());
            }
            
            dbus_message_unref(msg);
        }
        
        // Sleep a bit to avoid busy loop
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    LOG_INFO("D-Bus monitor thread exiting");
}

void DBusMonitor::processMessage(DBusMessage* msg) {
    if (!msg) return;
    
    // Check if this is a PrepareForSleep signal
    if (dbus_message_is_signal(msg, "org.freedesktop.login1.Manager", "PrepareForSleep")) {
        LOG_DEBUG("Received PrepareForSleep signal");
        
        DBusError error;
        dbus_error_init(&error);
        
        dbus_bool_t sleeping = FALSE;
        if (dbus_message_get_args(msg, &error, 
                                 DBUS_TYPE_BOOLEAN, &sleeping, 
                                 DBUS_TYPE_INVALID)) {
            
            if (m_callback) {
                if (sleeping) {
                    LOG_INFO("System is preparing to sleep");
                    
                    // Verify we have an inhibit lock for delaying sleep
                    if (m_inhibitFd < 0) {
                        LOG_WARNING("No inhibit lock when preparing for sleep, trying to get one now");
                        takeInhibitLock();
                    }
                    
                    // Notify the daemon about the sleep
                    m_callback(PowerState::Suspending);
                    
                    // We DON'T release the lock here - this will be done by
                    // the daemon after CEC operations are complete
                } else {
                    LOG_INFO("System is waking up");
                    m_callback(PowerState::Resuming);
                    
                    // Ensure we have an inhibit lock for future sleep events
                    if (m_inhibitFd < 0) {
                        takeInhibitLock();
                    }
                }
            }
        } 
        else {
            LOG_ERROR("Failed to parse PrepareForSleep args: ", error.message);
            dbus_error_free(&error);
        }
    }
}

} // namespace cec_control
