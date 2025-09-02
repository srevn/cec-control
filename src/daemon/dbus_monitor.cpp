#include "dbus_monitor.h"
#include "../common/logger.h"

#include <systemd/sd-bus.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>
#include <sys/select.h>

namespace cec_control {

DBusMonitor::DBusMonitor()
    : m_bus(nullptr),
      m_signalSlot(nullptr),
      m_inhibitFd(-1),
      m_running(false) {
    m_shutdownPipe[0] = -1;
    m_shutdownPipe[1] = -1;
}

DBusMonitor::~DBusMonitor() {
    stop();
    releaseInhibitLock();
    
    // Clean up shutdown pipe
    if (m_shutdownPipe[0] >= 0) close(m_shutdownPipe[0]);
    if (m_shutdownPipe[1] >= 0) close(m_shutdownPipe[1]);
    
    // Clean up D-Bus resources
    std::lock_guard<std::recursive_mutex> lock(m_busMutex);
    if (m_signalSlot) {
        sd_bus_slot_unref(m_signalSlot);
        m_signalSlot = nullptr;
    }
    
    if (m_bus) {
        sd_bus_unref(m_bus);
        m_bus = nullptr;
    }
}

bool DBusMonitor::initialize() {
    LOG_INFO("Initializing sd-bus D-Bus monitor");
    
    std::lock_guard<std::recursive_mutex> lock(m_busMutex);

    // Connect to system bus
    int r = sd_bus_default_system(&m_bus);
    if (r < 0) {
        LOG_ERROR("Failed to connect to system D-Bus: ", busErrorToString(r));
        return false;
    }
    
    // Add match for PrepareForSleep signal
    r = sd_bus_add_match(m_bus, &m_signalSlot,
        "type='signal',"
        "interface='org.freedesktop.login1.Manager',"
        "member='PrepareForSleep',"
        "path='/org/freedesktop/login1'",
        onPrepareForSleep, this);
    
    if (r < 0) {
        LOG_ERROR("Failed to add D-Bus signal match: ", busErrorToString(r));
        sd_bus_unref(m_bus);
        m_bus = nullptr;
        return false;
    }
    
    // Create shutdown pipe for thread communication
    if (pipe(m_shutdownPipe) < 0) {
        LOG_ERROR("Failed to create shutdown pipe: ", strerror(errno));
        sd_bus_unref(m_bus);
        m_bus = nullptr;
        return false;
    }
    
    // Take initial inhibitor lock
    if (!takeInhibitLock()) {
        LOG_WARNING("Failed to take initial inhibitor lock - sleep delays may not work properly");
    }
    
    LOG_INFO("sd-bus D-Bus monitor initialized successfully");
    return true;
}

void DBusMonitor::start(PowerStateCallback callback) {
    if (m_running) {
        LOG_WARNING("D-Bus monitor already running");
        return;
    }
    
    if (!m_bus) {
        LOG_ERROR("D-Bus connection not initialized");
        return;
    }
    
    m_callback = callback;
    m_running = true;
    
    // Start monitoring thread
    m_thread = std::thread(&DBusMonitor::eventLoop, this);
    
    LOG_INFO("sd-bus D-Bus monitor started");
}

void DBusMonitor::stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    
    LOG_INFO("Stopping sd-bus D-Bus monitor");
    
    // Signal thread to exit
    if (m_shutdownPipe[1] >= 0) {
        char buf = 1;
        if (write(m_shutdownPipe[1], &buf, 1) < 0) {
            LOG_WARNING("Failed to signal shutdown: ", strerror(errno));
        }
    }
    
    // Wait for thread to complete
    if (m_thread.joinable()) {
        LOG_DEBUG("Waiting for D-Bus monitor thread to exit");
        m_thread.join();
    }
    
    LOG_INFO("sd-bus D-Bus monitor stopped");
}

bool DBusMonitor::takeInhibitLock() {
    std::lock_guard<std::recursive_mutex> lock(m_busMutex);

    if (!m_bus) {
        LOG_ERROR("D-Bus connection not initialized");
        return false;
    }
    
    // If we already have a lock, don't take another
    if (m_inhibitFd >= 0) {
        LOG_DEBUG("Already have inhibitor lock (fd=", m_inhibitFd, ")");
        return true;
    }
    
    LOG_INFO("Taking systemd inhibitor lock");
    
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(m_bus,
        "org.freedesktop.login1",           // destination
        "/org/freedesktop/login1",          // path
        "org.freedesktop.login1.Manager",   // interface
        "Inhibit",                          // method
        &error,                             // error
        &reply,                             // reply
        "ssss",                             // signature
        "sleep",                            // what
        "cec-control",                      // who
        "Preparing CEC adapter for sleep",  // why
        "delay");                           // mode
    
    if (r < 0) {
        LOG_ERROR("Failed to call Inhibit method: ", error.message);
        sd_bus_error_free(&error);
        return false;
    }
    
    // Extract file descriptor from reply
    int temp_fd;
    r = sd_bus_message_read(reply, "h", &temp_fd);
    if (r < 0) {
        LOG_ERROR("Failed to read inhibitor file descriptor: ", busErrorToString(r));
        sd_bus_message_unref(reply);
        return false;
    }
    
    // Duplicate the file descriptor to keep it alive after message cleanup
    m_inhibitFd = dup(temp_fd);
    sd_bus_message_unref(reply);
    
    if (m_inhibitFd < 0) {
        LOG_ERROR("Failed to duplicate inhibitor file descriptor: ", strerror(errno));
        return false;
    }
    
    LOG_INFO("Successfully took inhibitor lock (fd=", m_inhibitFd, ")");
    return true;
}

bool DBusMonitor::releaseInhibitLock() {
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

void DBusMonitor::eventLoop() {
    LOG_INFO("sd-bus D-Bus monitor thread started");
    
    while (m_running) {
        int bus_fd;
        {
            std::lock_guard<std::recursive_mutex> lock(m_busMutex);
            if (!m_bus) break;
            bus_fd = sd_bus_get_fd(m_bus);
            if (bus_fd < 0) {
                LOG_ERROR("Failed to get bus file descriptor: ", busErrorToString(bus_fd));
                break;
            }
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(bus_fd, &readfds);
        FD_SET(m_shutdownPipe[0], &readfds);
        int max_fd = (bus_fd > m_shutdownPipe[0]) ? bus_fd : m_shutdownPipe[0];
        
        // Wait for events indefinitely until a signal is received
        int select_result = select(max_fd + 1, &readfds, nullptr, nullptr, nullptr);
        
        if (select_result < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("select() failed: ", strerror(errno));
            break;
        }
        
        // Check for shutdown signal
        if (FD_ISSET(m_shutdownPipe[0], &readfds)) {
            LOG_DEBUG("Received shutdown signal");
            break;
        }
        
        // Process D-Bus events if ready
        if (m_running && FD_ISSET(bus_fd, &readfds)) {
            std::lock_guard<std::recursive_mutex> lock(m_busMutex);
            if (!m_bus) break;

            int r;
            do {
                r = sd_bus_process(m_bus, nullptr);
                if (r < 0) {
                    LOG_ERROR("Failed to process D-Bus events: ", busErrorToString(r));
                    break;
                }
            } while (r > 0 && m_running);
        }
        
        // Handle any pending D-Bus operations
        if (m_running) {
            std::lock_guard<std::recursive_mutex> lock(m_busMutex);
            if (!m_bus) break;
            
            int r = sd_bus_flush(m_bus);
            if (r < 0 && r != -ENOTCONN) {
                LOG_WARNING("Failed to flush D-Bus connection: ", busErrorToString(r));
            }
        }
    }
    
    LOG_INFO("sd-bus D-Bus monitor thread exiting");
}

int DBusMonitor::onPrepareForSleep(sd_bus_message* msg, void* userdata, sd_bus_error* /*ret_error*/) {
    DBusMonitor* monitor = static_cast<DBusMonitor*>(userdata);
    if (!monitor) {
        LOG_ERROR("Invalid userdata in PrepareForSleep callback");
        return 0;
    }
    
    int sleeping = 0;
    int r = sd_bus_message_read(msg, "b", &sleeping);
    if (r < 0) {
        LOG_ERROR("Failed to read PrepareForSleep signal parameter: ", monitor->busErrorToString(r));
        return 0;
    }
    
    if (!monitor->m_callback) {
        LOG_WARNING("No callback registered for power state changes");
        return 0;
    }
    
    if (sleeping) {
        LOG_INFO("System is preparing to sleep");
        
        // Verify we have an inhibit lock
        if (monitor->m_inhibitFd < 0) {
            LOG_WARNING("No inhibit lock when preparing for sleep, trying to get one now");
            monitor->takeInhibitLock();
        }
        
        // Notify daemon about suspend
        monitor->m_callback(PowerState::Suspending);
    } else {
        LOG_INFO("System is waking up");
        
        // Notify daemon about resume
        monitor->m_callback(PowerState::Resuming);
        
        // Ensure we have an inhibit lock for future sleep events
        if (monitor->m_inhibitFd < 0) {
            monitor->takeInhibitLock();
        }
    }
    
    return 0;
}

bool DBusMonitor::suspendSystem() {
    std::lock_guard<std::recursive_mutex> lock(m_busMutex);

    if (!m_bus) {
        LOG_ERROR("D-Bus connection not initialized");
        return false;
    }
    
    LOG_INFO("Initiating system suspend via D-Bus");
    
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(m_bus,
        "org.freedesktop.login1",           // destination
        "/org/freedesktop/login1",          // path
        "org.freedesktop.login1.Manager",   // interface
        "Suspend",                          // method
        &error,                             // error
        &reply,                             // reply
        "b",                                // signature
        1);                                 // interactive=true
    
    if (r < 0) {
        LOG_ERROR("Failed to call Suspend method: ", error.message);
        sd_bus_error_free(&error);
        return false;
    }
    
    if (reply) {
        sd_bus_message_unref(reply);
    }
    
    LOG_INFO("System suspend initiated successfully via D-Bus");
    return true;
}

const char* DBusMonitor::busErrorToString(int error) {
    return strerror(-error);
}

} // namespace cec_control
