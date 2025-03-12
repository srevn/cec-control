#include "dbus_monitor.h"
#include "../common/logger.h"

#include <dbus/dbus.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <stdexcept>

namespace cec_control {

DBusMonitor::DBusMonitor() 
    : m_connection(nullptr),
      m_running(false) {
}

DBusMonitor::~DBusMonitor() {
    stop();
    
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
                    m_callback(PowerState::Suspending);
                } else {
                    LOG_INFO("System is waking up");
                    m_callback(PowerState::Resuming);
                }
            }
        } 
        else {
            LOG_ERROR("Failed to parse PrepareForSleep args: ", error.message);
            dbus_error_free(&error);
        }
    }
}

bool DBusMonitor::sendReadyForSleep() {
    if (!m_connection) {
        LOG_ERROR("D-Bus connection not initialized");
        return false;
    }
    
    LOG_INFO("Sending ReadyForSleep signal");
    
    DBusMessage* msg = dbus_message_new_method_call(
        "org.freedesktop.login1",         // destination
        "/org/freedesktop/login1",         // path
        "org.freedesktop.login1.Manager",  // interface
        "SleepWithInhibitors");            // method
        
    if (!msg) {
        LOG_ERROR("Failed to create D-Bus message");
        return false;
    }
    
    // Add the sleep string parameter
    const char* sleepParam = "sleep";
    if (!dbus_message_append_args(msg, 
                                DBUS_TYPE_STRING, &sleepParam, 
                                DBUS_TYPE_INVALID)) {
        LOG_ERROR("Failed to append args to D-Bus message");
        dbus_message_unref(msg);
        return false;
    }
    
    // Send the message
    DBusError error;
    dbus_error_init(&error);
    
    DBusPendingCall* pending = nullptr;
    if (!dbus_connection_send_with_reply(m_connection, msg, &pending, -1)) {
        LOG_ERROR("Failed to send D-Bus message");
        dbus_message_unref(msg);
        return false;
    }
    
    if (!pending) {
        LOG_ERROR("D-Bus pending call failed");
        dbus_message_unref(msg);
        return false;
    }
    
    // Block until we get a reply
    dbus_pending_call_block(pending);
    
    // Get the reply message
    DBusMessage* reply = dbus_pending_call_steal_reply(pending);
    dbus_pending_call_unref(pending);
    
    if (!reply) {
        LOG_ERROR("Failed to get D-Bus reply");
        dbus_message_unref(msg);
        return false;
    }
    
    // Check for errors
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        LOG_ERROR("D-Bus error: ", dbus_message_get_error_name(reply));
        dbus_message_unref(reply);
        dbus_message_unref(msg);
        return false;
    }
    
    // Clean up
    dbus_message_unref(reply);
    dbus_message_unref(msg);
    
    LOG_INFO("ReadyForSleep signal sent successfully");
    return true;
}

} // namespace cec_control
