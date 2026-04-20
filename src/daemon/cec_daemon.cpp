#include "cec_daemon.h"
#include "../common/logger.h"
#include "../common/systemd_env.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>
#include <utility>

#include "../common/event_poller.h"

namespace cec_control {

CECDaemon* CECDaemon::s_instance = nullptr;

CECDaemon::CECDaemon(Options daemonOptions, CommandRouter::Options routerOptions)
    : m_routerOptions(std::move(routerOptions)),
      m_threadPool(std::make_shared<ThreadPool>(4)),
      m_options(daemonOptions) {
    s_instance = this;
}

CECDaemon::~CECDaemon() {
    stop();
    s_instance = nullptr;
}

bool CECDaemon::start() {
    LOG_INFO("Starting CEC daemon");

    // Wake fd must be created before installing signal handlers, since the
    // handler writes into it. Non-blocking + close-on-exec.
    m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeFd < 0) {
        LOG_ERROR("Failed to create wakeup eventfd: ", std::strerror(errno));
        return false;
    }

    m_threadPool->start();

    try {
        m_router = std::make_unique<CommandRouter>(std::move(m_routerOptions), m_threadPool);
        m_router->setConnectionLostCallback([this]() { this->onConnectionLost(); });
        m_router->setSuspendCallback([this]() -> bool {
            return m_dbusMonitor ? m_dbusMonitor->suspendSystem() : false;
        });

        if (!m_router->initialize()) {
            LOG_ERROR("Failed to initialize command router");
            return false;
        }

        m_socketServer = std::make_unique<SocketServer>(m_threadPool);
        m_socketServer->setCommandHandler([this](const Message& cmd) {
            return this->handleCommand(cmd);
        });

        if (!m_socketServer->start()) {
            LOG_ERROR("Failed to start socket server");
            m_router->shutdown();
            return false;
        }

        setupSignalHandlers();

        if (m_options.enablePowerMonitor) {
            if (!setupPowerMonitor()) {
                LOG_WARNING("Failed to set up power monitoring. Sleep/wake events will not be handled automatically.");
            }
        } else {
            LOG_INFO("D-Bus power monitoring disabled via configuration. Suspend/resume operations will require manual commands.");
        }

        m_running.store(true, std::memory_order_release);
        LOG_INFO("CEC daemon started successfully");
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon startup: ", e.what());
        return false;
    }
}

void CECDaemon::stop() {
    if (!m_running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    LOG_INFO("Stopping CEC daemon");

    // Restore default signal handlers before any other teardown, so a signal
    // arriving mid-shutdown doesn't try to call into a half-destroyed daemon.
    teardownSignalHandlers();

    // Wake the run loop so it can exit on its next iteration. Safe even if
    // run() has already returned.
    wakeMainLoop();

    if (SystemdEnv::isUnderSystemd()) {
        LOG_INFO("Stopping daemon under systemd control");
    }

    try {
        if (m_dbusMonitor) {
            LOG_INFO("Stopping D-Bus monitor");
            m_dbusMonitor->stop();
        }

        if (m_socketServer) {
            LOG_INFO("Stopping socket server");
            const auto t0 = std::chrono::steady_clock::now();
            m_socketServer->stop();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Socket server stopped in ", ms, "ms");
        }

        if (m_router) {
            LOG_INFO("Shutting down command router");
            const auto t0 = std::chrono::steady_clock::now();
            m_router->shutdown();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Command router shutdown completed in ", ms, "ms");
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon shutdown: ", e.what());
    }

    LOG_INFO("Releasing resources");
    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_router.reset();

    if (m_threadPool) {
        LOG_INFO("Shutting down thread pool");
        m_threadPool->shutdown();
        m_threadPool.reset();
    }

    // Close wake fd after handlers are restored. Setting m_wakeFd to -1
    // before close ensures any concurrent (best-effort) signal handler
    // observes the invalid fd and skips its write().
    if (int fd = std::exchange(m_wakeFd, -1); fd >= 0) {
        ::close(fd);
    }

    LOG_INFO("CEC daemon stopped - shutdown sequence complete");
}

void CECDaemon::run() {
    LOG_INFO("Entering main daemon loop");

    if (m_wakeFd < 0) {
        LOG_ERROR("Wake fd not initialised; main loop cannot run");
        return;
    }

    EventPoller poller;
    if (!poller.add(m_wakeFd, static_cast<uint32_t>(EventPoller::Event::READ))) {
        LOG_ERROR("Failed to register wake fd with event poller");
        return;
    }

    while (m_running.load(std::memory_order_acquire)) {
        // Block until a wakeup arrives (signal handler, onConnectionLost,
        // or stop()). EINTR yields an empty event vector; nullopt means the
        // poller is unusable and we should exit so systemd can restart us.
        auto events = poller.wait(-1);
        if (!events) {
            LOG_ERROR("Event poller failed; main loop exiting");
            m_running.store(false, std::memory_order_release);
            break;
        }
        drainWakeFd();

        if (!m_running.load(std::memory_order_acquire)) {
            break;
        }

        if (m_connectionLost.exchange(false, std::memory_order_acq_rel)) {
            onConnectionLostEvent();
        }
    }

    LOG_DEBUG("Main daemon loop exited");
}

void CECDaemon::onConnectionLost() {
    LOG_INFO("CEC connection lost event received");
    m_connectionLost.store(true, std::memory_order_release);
    wakeMainLoop();
}

void CECDaemon::onConnectionLostEvent() {
    if (!m_router) return;
    LOG_WARNING("CEC connection lost, attempting to reconnect");
    if (m_router->reconnect()) {
        LOG_INFO("Successfully reconnected to CEC adapter");
    } else {
        LOG_ERROR("Failed to reconnect to CEC adapter - will retry on next event");
    }
}

void CECDaemon::wakeMainLoop() noexcept {
    const int fd = m_wakeFd;
    if (fd < 0) return;
    const uint64_t one = 1;
    ssize_t r = ::write(fd, &one, sizeof(one));
    (void)r;  // Best-effort: if the eventfd is full or closed we have nothing useful to do.
}

void CECDaemon::drainWakeFd() noexcept {
    if (m_wakeFd < 0) return;
    uint64_t scratch;
    while (::read(m_wakeFd, &scratch, sizeof(scratch)) > 0) {
        // eventfd is a counter; one read returns the current sum and zeros it.
    }
}

void CECDaemon::onSuspend() {
    if (!m_router) return;
    LOG_INFO("System suspending, preparing CEC adapter");

    // Safety net: if router->suspend() hangs (libcec misbehaving, USB stuck),
    // release the DBus inhibit lock anyway so the system isn't stuck refusing
    // to sleep indefinitely. DBusMonitor::releaseInhibitLock is idempotent,
    // so a late-completion path releasing it a second time is harmless.
    auto completionPromise = std::make_shared<std::promise<void>>();
    std::shared_future<void> completionFuture(completionPromise->get_future());
    m_threadPool->submit([this, completionFuture]() {
        if (completionFuture.wait_for(std::chrono::seconds(10)) ==
            std::future_status::timeout) {
            LOG_WARNING("Suspend did not complete within 10s; releasing inhibit lock forcibly");
            if (m_dbusMonitor) m_dbusMonitor->releaseInhibitLock();
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    try {
        m_router->suspend();
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during suspend: ", e.what());
    }
    try {
        completionPromise->set_value();
    } catch (const std::future_error&) {
        // Safety task already consumed the promise (timeout path); nothing to do.
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("CEC suspend took ", ms, "ms");

    if (m_dbusMonitor) {
        LOG_INFO("CEC sleep preparation complete; allowing system to sleep");
        m_dbusMonitor->releaseInhibitLock();
    }
}

void CECDaemon::onResume() {
    if (!m_router) return;
    LOG_INFO("System resuming, reinitializing CEC adapter");

    try {
        m_router->resume();
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during resume: ", e.what());
    }

    // After resume, the USB subsystem can still be settling. One delayed
    // retry ten seconds later covers the window where the first reopen races
    // with ttyACM* re-enumeration. Subsequent failures stay stuck in
    // "RESP_ERROR mode" until the operator issues `restart` or the adapter
    // re-asserts — we deliberately do not escalate.
    if (!m_router->isAdapterValid()) {
        m_threadPool->submit([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            if (m_router && !m_router->isAdapterValid() && !m_router->isSuspended()) {
                LOG_INFO("Performing delayed reconnection attempt");
                (void)m_router->reconnect();
            }
        });
    }
}

bool CECDaemon::setupPowerMonitor() {
    LOG_INFO("Setting up D-Bus power monitoring");
    try {
        m_dbusMonitor = std::make_unique<DBusMonitor>();
        if (!m_dbusMonitor->initialize()) {
            LOG_ERROR("Failed to initialize D-Bus monitor");
            return false;
        }
        m_dbusMonitor->start([this](DBusMonitor::PowerState state) {
            this->handlePowerStateChange(state);
        });
        LOG_INFO("D-Bus power monitoring setup successfully");
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during D-Bus power monitor setup: ", e.what());
        return false;
    }
}

void CECDaemon::handlePowerStateChange(DBusMonitor::PowerState state) {
    switch (state) {
        case DBusMonitor::PowerState::Suspending:
            LOG_INFO("Received system suspend notification from D-Bus");
            onSuspend();
            break;
        case DBusMonitor::PowerState::Resuming:
            LOG_INFO("Received system resume notification from D-Bus");
            onResume();
            break;
    }
}

Message CECDaemon::handleCommand(const Message& command) {
    LOG_DEBUG("Received command: type=", static_cast<int>(command.type),
              ", deviceId=", static_cast<int>(command.deviceId));

    if (command.type == MessageType::CMD_SUSPEND) {
        LOG_INFO("Processing suspend command");
        onSuspend();
        return Message(MessageType::RESP_SUCCESS);
    }
    if (command.type == MessageType::CMD_RESUME) {
        LOG_INFO("Processing resume command");
        onResume();
        return Message(MessageType::RESP_SUCCESS);
    }

    if (!m_router) {
        LOG_ERROR("Command router not initialized");
        return Message(MessageType::RESP_ERROR);
    }

    return m_router->dispatch(command);
}

void CECDaemon::setupSignalHandlers() {
    // sigaction is preferred over std::signal: well-defined semantics across
    // POSIX implementations, and we explicitly want signals to interrupt
    // blocking syscalls (no SA_RESTART) so the run loop wakes deterministically.
    struct sigaction sa{};
    sa.sa_handler = &CECDaemon::signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
}

void CECDaemon::teardownSignalHandlers() {
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);
}

void CECDaemon::signalHandler(int /*signum*/) {
    // Strictly async-signal-safe: only atomic stores, write() on an eventfd,
    // and _exit(). No logging, no condition variables, no allocator calls.
    CECDaemon* instance = s_instance;
    if (!instance) {
        _exit(EXIT_FAILURE);
    }

    instance->m_running.store(false, std::memory_order_release);

    const int fd = instance->m_wakeFd;
    if (fd >= 0) {
        const uint64_t one = 1;
        ssize_t r = ::write(fd, &one, sizeof(one));
        (void)r;  // Best-effort wake; the loop will re-check m_running anyway.
    }

    // Escalate to forced exit on the third signal so a wedged shutdown can
    // still be aborted by a determined operator.
    if (instance->m_signalCount.fetch_add(1, std::memory_order_relaxed) >= 2) {
        _exit(EXIT_FAILURE);
    }
}

} // namespace cec_control
