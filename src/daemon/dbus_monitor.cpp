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
        return false;
    }
    
    // Create shutdown pipe for thread communication
    if (pipe(m_shutdownPipe) < 0) {
        LOG_ERROR("Failed to create shutdown pipe: ", strerror(errno));
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
    int r = sd_bus_call_method(m_bus,
        "org.freedesktop.login1",           // destination
        "/org/freedesktop/login1",          // path
        "org.freedesktop.login1.Manager",   // interface
        "Inhibit",                          // method
        nullptr,                            // error
        &reply,                             // reply
        "ssss",                             // signature
        "sleep",                            // what
        "cec-control",                      // who
        "Preparing CEC adapter for sleep",  // why
        "delay");                           // mode
    
    if (r < 0) {
        LOG_ERROR("Failed to call Inhibit method: ", busErrorToString(r));
        return false;
    }
    
    // Extract file descriptor from reply
    r = sd_bus_message_read(reply, "h", &m_inhibitFd);
    sd_bus_message_unref(reply);
    
    if (r < 0) {
        LOG_ERROR("Failed to read inhibitor file descriptor: ", busErrorToString(r));
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
        // Check for shutdown signal
        fd_set readfds;
        FD_ZERO(&readfds);
        
        int bus_fd = sd_bus_get_fd(m_bus);
        if (bus_fd < 0) {
            LOG_ERROR("Failed to get bus file descriptor: ", busErrorToString(bus_fd));
            break;
        }
        
        FD_SET(bus_fd, &readfds);
        FD_SET(m_shutdownPipe[0], &readfds);
        int max_fd = (bus_fd > m_shutdownPipe[0]) ? bus_fd : m_shutdownPipe[0];
        
        // Wait for events
        struct timeval timeout = {1, 0};  // 1 second timeout
        int select_result = select(max_fd + 1, &readfds, nullptr, nullptr, &timeout);
        
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
        if (select_result > 0 && FD_ISSET(bus_fd, &readfds)) {
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

const char* DBusMonitor::busErrorToString(int error) {
    return strerror(-error);
}

} // namespace cec_control