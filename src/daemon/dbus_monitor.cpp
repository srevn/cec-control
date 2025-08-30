#include "dbus_monitor.h"
#include "../common/logger.h"
#include "../common/event_poller.h"

#include <dbus/dbus.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

namespace cec_control {

DBusMonitor::DBusMonitor() 
    : m_connection(nullptr),
      m_running(false),
      m_inhibitFd(-1) {
}

DBusMonitor::~DBusMonitor() {
    stop();
    releaseInhibitLock();
    
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
    
    // Add match rule for PrepareForSleep signal
    dbus_bus_add_match(m_connection, 
        "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'", 
        &error);
        
    if (dbus_error_is_set(&error)) {
        LOG_ERROR("Failed to add D-Bus match rule: ", error.message);
        dbus_error_free(&error);
        return false;
    }
    
    // Add message filter to intercept signals
    if (!dbus_connection_add_filter(m_connection, messageFilterCallback, this, nullptr)) {
        LOG_ERROR("Failed to add D-Bus message filter");
        return false;
    }
    
    // Set up watch and timeout functions
    if (!dbus_connection_set_watch_functions(
            m_connection,
            addWatchCallback, 
            removeWatchCallback,
            toggleWatchCallback,
            this,  // user data
            nullptr)) {
        LOG_ERROR("Failed to set watch functions");
        return false;
    }
    
    if (!dbus_connection_set_timeout_functions(
            m_connection,
            addTimeoutCallback,
            removeTimeoutCallback,
            toggleTimeoutCallback,
            this,  // user data
            nullptr)) {
        LOG_ERROR("Failed to set timeout functions");
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
    
    // Start monitoring thread with event-driven approach
    m_thread = std::thread(&DBusMonitor::monitorLoop, this);
    
    LOG_INFO("D-Bus monitor started with event-driven approach");
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
    
    // Add arguments for the Inhibit call
    const char* what = "sleep";
    const char* who = "cec-control";
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
    
    LOG_INFO("D-Bus monitor thread started with event-driven approach");
    
    // Create event poller
    EventPoller poller;
    
    while (m_running) {
        // Update watches
        {
            std::lock_guard<std::mutex> lock(m_watchMutex);
            
            // First remove any watches that have been removed from m_watches
            for (auto it = m_fdToPoller.begin(); it != m_fdToPoller.end();) {
                bool found = false;
                for (const auto& watch : m_watches) {
                    if (watch.fd == it->first && watch.enabled) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    poller.remove(it->first);
                    it = m_fdToPoller.erase(it);
                } else {
                    ++it;
                }
            }
            
            // Then add or update watches
            for (const auto& watchInfo : m_watches) {
                if (!watchInfo.enabled) {
                    auto it = m_fdToPoller.find(watchInfo.fd);
                    if (it != m_fdToPoller.end()) {
                        poller.remove(watchInfo.fd);
                        m_fdToPoller.erase(it);
                    }
                    continue;
                }
                
                uint32_t events = 0;
                if (watchInfo.flags & DBUS_WATCH_READABLE) {
                    events |= static_cast<uint32_t>(EventPoller::Event::READ);
                }
                if (watchInfo.flags & DBUS_WATCH_WRITABLE) {
                    events |= static_cast<uint32_t>(EventPoller::Event::WRITE);
                }
                
                auto it = m_fdToPoller.find(watchInfo.fd);
                if (it == m_fdToPoller.end()) {
                    // New fd to add
                    if (poller.add(watchInfo.fd, events)) {
                        m_fdToPoller[watchInfo.fd] = events;
                    }
                } else if (it->second != events) {
                    // Existing fd with changed events
                    if (poller.modify(watchInfo.fd, events)) {
                        it->second = events;
                    }
                }
            }
        }
        
        // Calculate timeout - default to 100ms if no timeouts
        int maxTimeout = 100;
        
        {
            std::lock_guard<std::mutex> lock(m_timeoutMutex);
            if (!m_timeouts.empty()) {
                struct timeval tv;
                gettimeofday(&tv, nullptr);
                int64_t now = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                
                int64_t earliest = INT64_MAX;
                bool hasTimeouts = false;
                
                for (const auto& timeoutInfo : m_timeouts) {
                    if (timeoutInfo.enabled) {
                        hasTimeouts = true;
                        if (timeoutInfo.expiry < earliest) {
                            earliest = timeoutInfo.expiry;
                        }
                    }
                }
                
                if (hasTimeouts) {
                    maxTimeout = (earliest <= now) ? 0 : static_cast<int>(earliest - now);
                }
            }
        }
        
        // Wait for events with the calculated timeout
        auto events = poller.wait(maxTimeout);
        
        // Handle watches that have activity
        if (!events.empty()) {
            std::lock_guard<std::mutex> lock(m_watchMutex);
            for (const auto& event : events) {
                for (const auto& watchInfo : m_watches) {
                    if (watchInfo.enabled && watchInfo.fd == event.fd) {
                        unsigned int flags = 0;
                        if (event.events & static_cast<uint32_t>(EventPoller::Event::READ)) {
                            flags |= DBUS_WATCH_READABLE;
                        }
                        if (event.events & static_cast<uint32_t>(EventPoller::Event::WRITE)) {
                            flags |= DBUS_WATCH_WRITABLE;
                        }
                        if (event.events & static_cast<uint32_t>(EventPoller::Event::ERROR)) {
                            flags |= DBUS_WATCH_ERROR;
                        }
                        
                        if (flags != 0) {
                            dbus_watch_handle(watchInfo.watch, flags);
                        }
                        break;
                    }
                }
            }
        }
        
        // Handle timeouts that have expired
        {
            std::lock_guard<std::mutex> lock(m_timeoutMutex);
            if (!m_timeouts.empty()) {
                struct timeval tv;
                gettimeofday(&tv, nullptr);
                int64_t now = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
                
                for (auto& timeoutInfo : m_timeouts) {
                    if (timeoutInfo.enabled && timeoutInfo.expiry <= now) {
                        dbus_timeout_handle(timeoutInfo.timeout);
                        // Update expiry time for next interval
                        updateTimeoutExpiry(timeoutInfo);
                    }
                }
            }
        }
        
        // Dispatch any pending messages
        while (dbus_connection_dispatch(m_connection) == DBUS_DISPATCH_DATA_REMAINS);
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
        if (!dbus_message_get_args(msg, &error, 
                                 DBUS_TYPE_BOOLEAN, &sleeping, 
                                 DBUS_TYPE_INVALID)) {
            LOG_ERROR("Failed to parse PrepareForSleep args: ", error.message);
            dbus_error_free(&error);
            return;
        }
        
        if (!m_callback) return;
        
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

// Update expiry time for a timeout
void DBusMonitor::updateTimeoutExpiry(TimeoutInfo& info) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    info.expiry = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 + info.interval;
}

// D-Bus watch callback implementations
dbus_bool_t DBusMonitor::addWatchCallback(DBusWatch* watch, void* data) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(data);
    if (!monitor) return FALSE;
    
    int fd = dbus_watch_get_unix_fd(watch);
    unsigned int flags = dbus_watch_get_flags(watch);
    bool enabled = dbus_watch_get_enabled(watch);
    
    LOG_DEBUG("Adding D-Bus watch for fd ", fd, ", flags: ", flags, ", enabled: ", enabled);
    
    std::lock_guard<std::mutex> lock(monitor->m_watchMutex);
    monitor->m_watches.push_back({watch, fd, flags, enabled});
    return TRUE;
}

void DBusMonitor::removeWatchCallback(DBusWatch* watch, void* data) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(data);
    if (!monitor) return;
    
    int fd = dbus_watch_get_unix_fd(watch);
    LOG_DEBUG("Removing D-Bus watch for fd ", fd);
    
    std::lock_guard<std::mutex> lock(monitor->m_watchMutex);
    auto it = std::find_if(monitor->m_watches.begin(), monitor->m_watches.end(),
                          [watch](const WatchInfo& info) { return info.watch == watch; });
    
    if (it != monitor->m_watches.end()) {
        monitor->m_watches.erase(it);
    }
}

void DBusMonitor::toggleWatchCallback(DBusWatch* watch, void* data) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(data);
    if (!monitor) return;
    
    bool enabled = dbus_watch_get_enabled(watch);
    
    std::lock_guard<std::mutex> lock(monitor->m_watchMutex);
    auto it = std::find_if(monitor->m_watches.begin(), monitor->m_watches.end(),
                          [watch](const WatchInfo& info) { return info.watch == watch; });
    
    if (it != monitor->m_watches.end()) {
        LOG_DEBUG("Toggling D-Bus watch for fd ", it->fd, " to ", enabled);
        it->enabled = enabled;
    }
}

// D-Bus timeout callback implementations
dbus_bool_t DBusMonitor::addTimeoutCallback(DBusTimeout* timeout, void* data) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(data);
    if (!monitor) return FALSE;
    
    int interval = dbus_timeout_get_interval(timeout);
    bool enabled = dbus_timeout_get_enabled(timeout);
    
    LOG_DEBUG("Adding D-Bus timeout with interval ", interval, "ms, enabled: ", enabled);
    
    std::lock_guard<std::mutex> lock(monitor->m_timeoutMutex);
    TimeoutInfo info = {timeout, interval, enabled, 0};
    
    if (enabled) {
        monitor->updateTimeoutExpiry(info);
    }
    
    monitor->m_timeouts.push_back(info);
    return TRUE;
}

void DBusMonitor::removeTimeoutCallback(DBusTimeout* timeout, void* data) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(data);
    if (!monitor) return;
    
    std::lock_guard<std::mutex> lock(monitor->m_timeoutMutex);
    auto it = std::find_if(monitor->m_timeouts.begin(), monitor->m_timeouts.end(),
                          [timeout](const TimeoutInfo& info) { return info.timeout == timeout; });
    
    if (it != monitor->m_timeouts.end()) {
        LOG_DEBUG("Removing D-Bus timeout with interval ", it->interval, "ms");
        monitor->m_timeouts.erase(it);
    }
}

void DBusMonitor::toggleTimeoutCallback(DBusTimeout* timeout, void* data) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(data);
    if (!monitor) return;
    
    bool enabled = dbus_timeout_get_enabled(timeout);
    
    std::lock_guard<std::mutex> lock(monitor->m_timeoutMutex);
    auto it = std::find_if(monitor->m_timeouts.begin(), monitor->m_timeouts.end(),
                          [timeout](const TimeoutInfo& info) { return info.timeout == timeout; });
    
    if (it != monitor->m_timeouts.end()) {
        LOG_DEBUG("Toggling D-Bus timeout with interval ", it->interval, "ms to ", enabled);
        it->enabled = enabled;
        
        if (enabled) {
            monitor->updateTimeoutExpiry(*it);
        }
    }
}

// Message filter implementation
DBusHandlerResult DBusMonitor::messageFilterCallback(DBusConnection*, DBusMessage* message, void* user_data) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(user_data);
    if (monitor) {
        monitor->processMessage(message);
    }
    
    // Return "not yet handled" to allow other handlers to process this message too
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

} // namespace cec_control