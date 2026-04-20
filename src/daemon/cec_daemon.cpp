#include "cec_daemon.h"
#include "../common/logger.h"
#include "../common/systemd_env.h"

#include <array>
#include <chrono>
#include <csignal>
#include <utility>

namespace cec_control {

namespace {

constexpr auto kSuspendSafetyDeadline = std::chrono::seconds(10);
constexpr auto kResumeRetryDelay      = std::chrono::seconds(10);

/**
 * Retry schedule for CEC reconnect after a connection-lost event: the
 * libCEC alert triggers attempt #0 immediately; each subsequent entry
 * is the delay before the next attempt. A flaky USB-to-CEC bridge can
 * take a few seconds to re-enumerate, which the single prior retry
 * occasionally missed. Cancelled on suspend/resume; reset on any
 * successful reopen.
 */
constexpr std::array<std::chrono::milliseconds, 3> kConnectionLostRetrySchedule = {
    std::chrono::milliseconds(5'000),
    std::chrono::milliseconds(10'000),
    std::chrono::milliseconds(20'000),
};

} // namespace

CECDaemon::CECDaemon(Options daemonOptions, CommandRouter::Options routerOptions)
    : m_signals{SIGINT, SIGTERM, SIGHUP},
      m_threadPool(std::make_shared<ThreadPool>(4)),
      m_routerOptions(std::move(routerOptions)),
      m_options(daemonOptions) {}

CECDaemon::~CECDaemon() {
    stop();
}

bool CECDaemon::start() {
    LOG_INFO("Starting CEC daemon");

    if (!m_signals.valid()) {
        LOG_ERROR("Signal source not initialised; aborting start");
        return false;
    }
    if (!m_work.valid()) {
        LOG_ERROR("Main-thread work queue not initialised; aborting start");
        return false;
    }
    if (!m_suspendSafetyTimer.valid() || !m_resumeRetryTimer.valid()
            || !m_reconnectRetryTimer.valid()) {
        LOG_ERROR("Timer source(s) not initialised; aborting start");
        return false;
    }

    // Pool workers inherit the main thread's signal mask, which was set in
    // the SignalSource member's constructor. Start the pool after masking
    // and before spawning any other threads so SIGINT/SIGTERM/SIGHUP are
    // only delivered to us via signalfd on the main thread.
    m_threadPool->start();

    try {
        m_router = std::make_unique<CommandRouter>(std::move(m_routerOptions), m_threadPool);

        m_router->setConnectionLostCallback([this]() {
            // Runs on a libCEC-owned thread. Hand off to the main loop
            // so the retry state machine (counter, timer) is touched by
            // a single thread only.
            m_work.post([this]() { this->onConnectionLost(); });
        });

        m_router->setSuspendCallback([this]() {
            // Router calls this on a pool worker; sd-bus is single-owner
            // (main thread), so we post the call via the work queue. The
            // actual Suspend() success/failure is logged by DBusMonitor,
            // so this side is fire-and-forget.
            m_work.post([this]() {
                if (m_dbusMonitor) m_dbusMonitor->suspendSystem();
            });
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

        if (m_options.enablePowerMonitor) {
            if (!setupPowerMonitor()) {
                LOG_WARNING("Failed to set up power monitoring. Sleep/wake events will not be handled automatically.");
            }
        } else {
            LOG_INFO("D-Bus power monitoring disabled via configuration. Suspend/resume operations will require manual commands.");
        }

        // Register every fd source with the loop. Any failure here leaves
        // earlier-registered sources in place; stop() will tear them down
        // cleanly via EventLoop destruction.
        const auto READ = static_cast<uint32_t>(EventPoller::Event::READ);

        if (!m_loop.add(m_signals.fd(), READ,
                        [this](uint32_t) { this->onSignalReadable(); })) {
            LOG_ERROR("Failed to register signalfd with event loop");
            return false;
        }
        if (!m_loop.add(m_work.fd(), READ,
                        [this](uint32_t) { m_work.drain(); })) {
            LOG_ERROR("Failed to register work-queue fd with event loop");
            return false;
        }
        if (!m_loop.add(m_socketServer->listenerFd(), READ,
                        [this](uint32_t) { m_socketServer->onReadable(); })) {
            LOG_ERROR("Failed to register socket listener with event loop");
            return false;
        }
        if (!m_loop.add(m_suspendSafetyTimer.fd(), READ,
                        [this](uint32_t) { this->onSuspendSafetyTimer(); })) {
            LOG_ERROR("Failed to register suspend-safety timer with event loop");
            return false;
        }
        if (!m_loop.add(m_resumeRetryTimer.fd(), READ,
                        [this](uint32_t) { this->onResumeRetryTimer(); })) {
            LOG_ERROR("Failed to register resume-retry timer with event loop");
            return false;
        }
        if (!m_loop.add(m_reconnectRetryTimer.fd(), READ,
                        [this](uint32_t) { this->onReconnectRetryTimer(); })) {
            LOG_ERROR("Failed to register reconnect-retry timer with event loop");
            return false;
        }

        if (m_dbusMonitor) {
            if (!m_dbusMonitor->attach(m_loop)) {
                LOG_WARNING("Failed to attach D-Bus monitor to event loop");
                m_dbusMonitor.reset();
            }
        }

        m_started = true;
        LOG_INFO("CEC daemon started successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon startup: ", e.what());
        return false;
    }
}

void CECDaemon::run() {
    LOG_INFO("Entering main daemon loop");
    m_loop.run();
    LOG_DEBUG("Main daemon loop exited");
}

void CECDaemon::stop() {
    if (!m_started) return;
    m_started = false;

    LOG_INFO("Stopping CEC daemon");

    // Idempotent even if run() already returned on its own.
    m_loop.stop();

    if (SystemdEnv::isUnderSystemd()) {
        LOG_INFO("Stopping daemon under systemd control");
    }

    try {
        if (m_dbusMonitor) {
            m_dbusMonitor->detach();
        }

        // Each subsystem logs its own "Stopping" / "Stopped" lifecycle; the
        // daemon adds wall-clock timing so slow teardowns surface at a
        // glance without double-logging the transitions themselves.
        if (m_socketServer) {
            const auto t0 = std::chrono::steady_clock::now();
            m_socketServer->stop();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Socket server teardown: ", ms, "ms");
        }

        if (m_router) {
            const auto t0 = std::chrono::steady_clock::now();
            m_router->shutdown();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Command router teardown: ", ms, "ms");
        }

        if (m_dbusMonitor) {
            m_dbusMonitor->stop();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon shutdown: ", e.what());
    }

    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_router.reset();

    if (m_threadPool) {
        m_threadPool->shutdown();
        m_threadPool.reset();
    }

    LOG_INFO("CEC daemon stopped - shutdown sequence complete");
}

void CECDaemon::onSignalReadable() {
    while (auto info = m_signals.readOne()) {
        const int signum = static_cast<int>(info->ssi_signo);
        ++m_terminationSignalCount;
        LOG_INFO("Received signal ", signum, " (count=", m_terminationSignalCount, ")");

        // First signal requests a clean shutdown. Further signals are
        // visible in the log but the loop has already been asked to exit;
        // a truly stuck shutdown is resolved by systemd's TimeoutStopSec
        // (which ultimately delivers SIGKILL).
        m_loop.stop();
    }
}

void CECDaemon::enqueuePowerEvent(PowerEvent event, EventSource source) {
    m_pendingEvents.push_back({event, source});
    startNextLifecycleEvent();
}

void CECDaemon::startNextLifecycleEvent() {
    if (m_phase != LifecyclePhase::Idle) return;
    if (m_pendingEvents.empty()) return;

    const auto next = m_pendingEvents.front();
    m_pendingEvents.pop_front();
    m_phaseSource = next.source;

    switch (next.event) {
        case PowerEvent::Suspend: startSuspend(next.source); break;
        case PowerEvent::Resume:  startResume(next.source);  break;
    }
}

void CECDaemon::startSuspend(EventSource /*source*/) {
    if (!m_router) {
        // Daemon is mid-teardown; discard silently and keep draining so
        // the sequencer never wedges the queue.
        startNextLifecycleEvent();
        return;
    }
    LOG_INFO("System suspending, preparing CEC adapter");
    m_phase = LifecyclePhase::Suspending;

    // Cancel any pending post-resume retry: a new suspend invalidates it.
    m_resumeRetryTimer.disarm();
    m_resumeRetryPending = false;

    // Cancel any pending connection-lost retry: suspend closes the
    // adapter deliberately, so further reconnect attempts would just
    // race the suspend path. resume() will reopen the adapter itself.
    m_reconnectRetryTimer.disarm();
    m_reconnectAttempts = 0;

    // Arm the safety deadline so we never leave the system wedged waiting
    // on our inhibit lock if router->suspend() hangs.
    m_suspendSafetyArmed = true;
    if (!m_suspendSafetyTimer.armOnce(std::chrono::duration_cast<std::chrono::milliseconds>(
            kSuspendSafetyDeadline))) {
        LOG_WARNING("Could not arm suspend-safety timer; inhibit lock will only release on completion");
    }

    // Run the CEC-side suspend on a pool worker so the main loop continues
    // to service signals, sockets, and DBus while we wait.
    m_threadPool->submit([this]() {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            m_router->suspend();
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during suspend: ", e.what());
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        m_work.post([this, elapsed]() { this->onSuspendComplete(elapsed); });
    });
}

void CECDaemon::onSuspendComplete(std::chrono::milliseconds workDuration) {
    // Whichever path (completion vs. safety timer) fires first releases
    // the inhibit lock; the other becomes a no-op.
    m_suspendSafetyTimer.disarm();
    if (m_suspendSafetyArmed) {
        m_suspendSafetyArmed = false;
        LOG_INFO("CEC suspend took ", workDuration.count(), "ms");
        if (m_phaseSource == EventSource::DBus && m_dbusMonitor) {
            LOG_INFO("CEC sleep preparation complete; allowing system to sleep");
            m_dbusMonitor->releaseInhibitLock();
        }
    } else {
        LOG_WARNING("CEC suspend completed in ", workDuration.count(),
                    "ms but safety timer had already released the inhibit lock");
    }

    m_phase = LifecyclePhase::Idle;
    startNextLifecycleEvent();
}

void CECDaemon::startResume(EventSource /*source*/) {
    if (!m_router) {
        startNextLifecycleEvent();
        return;
    }
    LOG_INFO("System resuming, reinitializing CEC adapter");
    m_phase = LifecyclePhase::Resuming;

    // Clear any stale retry flag (another resume cycle starts fresh).
    m_resumeRetryTimer.disarm();
    m_resumeRetryPending = false;

    // resume() drives its own reopen; a connection-lost retry cycle
    // from before suspend is now stale.
    m_reconnectRetryTimer.disarm();
    m_reconnectAttempts = 0;

    m_threadPool->submit([this]() {
        try {
            m_router->resume();
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during resume: ", e.what());
        }
        const bool adapterValid = m_router->isAdapterValid();
        m_work.post([this, adapterValid]() { this->onResumeComplete(adapterValid); });
    });
}

void CECDaemon::onResumeComplete(bool adapterValid) {
    // After resume, the USB subsystem can still be settling. One delayed
    // retry ten seconds later covers the window where the first reopen
    // races with ttyACM* re-enumeration. Cancellable (disarmed on the
    // next suspend/resume).
    if (!adapterValid && !m_resumeRetryPending) {
        m_resumeRetryPending = true;
        if (!m_resumeRetryTimer.armOnce(std::chrono::duration_cast<std::chrono::milliseconds>(
                kResumeRetryDelay))) {
            LOG_WARNING("Could not arm resume-retry timer");
            m_resumeRetryPending = false;
        }
    }

    // Retake the delay-inhibitor so the next real sleep is guarded
    // again. Wire-sourced resumes never dropped the lock, so there is
    // nothing to retake in that case.
    if (m_phaseSource == EventSource::DBus && m_dbusMonitor) {
        m_dbusMonitor->takeInhibitLock();
    }

    m_phase = LifecyclePhase::Idle;
    startNextLifecycleEvent();
}

void CECDaemon::onSuspendSafetyTimer() {
    m_suspendSafetyTimer.consume();
    if (!m_suspendSafetyArmed) {
        // Race: completion path fired first and cleared the flag. The
        // disarm() happens-before the consume() above, but the fd could
        // still report one leftover expiration; nothing to do.
        return;
    }
    m_suspendSafetyArmed = false;
    LOG_WARNING("Suspend did not complete within ",
                kSuspendSafetyDeadline.count(),
                "s; releasing inhibit lock forcibly");
    // Only a D-Bus-sourced suspend ever held the lock; wire-sourced
    // suspends leave it untouched so the next real sleep stays guarded.
    if (m_phaseSource == EventSource::DBus && m_dbusMonitor) {
        m_dbusMonitor->releaseInhibitLock();
    }
}

void CECDaemon::onResumeRetryTimer() {
    m_resumeRetryTimer.consume();
    if (!m_resumeRetryPending) return;
    m_resumeRetryPending = false;

    m_threadPool->submit([this]() {
        if (!m_router) return;
        if (m_router->isAdapterValid() || m_router->isSuspended()) return;
        LOG_INFO("Performing delayed reconnection attempt");
        (void)m_router->reconnect();
    });
}

void CECDaemon::onConnectionLost() {
    LOG_WARNING("CEC connection lost, attempting to reconnect");
    m_reconnectRetryTimer.disarm();
    m_reconnectAttempts = 0;
    submitReconnectAttempt();
}

void CECDaemon::submitReconnectAttempt() {
    m_threadPool->submit([this]() {
        const bool ok = m_router && m_router->reconnect();
        m_work.post([this, ok]() { this->onReconnectResult(ok); });
    });
}

void CECDaemon::onReconnectResult(bool ok) {
    // A suspend cycle overlapping with an in-flight attempt leaves
    // m_router->isSuspended() true; the reconnect result is stale and
    // resume() handles the reopen. Drop the result silently.
    if (m_router && m_router->isSuspended()) {
        m_reconnectAttempts = 0;
        return;
    }
    if (ok) {
        LOG_INFO("Successfully reconnected to CEC adapter");
        m_reconnectAttempts = 0;
        return;
    }

    if (m_reconnectAttempts >= kConnectionLostRetrySchedule.size()) {
        LOG_WARNING("CEC reconnect abandoned after ",
                    m_reconnectAttempts + 1,
                    " attempts; waiting for next connection-lost event");
        m_reconnectAttempts = 0;
        return;
    }

    const auto delay = kConnectionLostRetrySchedule[m_reconnectAttempts];
    ++m_reconnectAttempts;
    LOG_WARNING("CEC reconnect attempt failed; next retry in ",
                delay.count(), "ms");
    if (!m_reconnectRetryTimer.armOnce(delay)) {
        LOG_ERROR("Failed to arm reconnect-retry timer; giving up this cycle");
        m_reconnectAttempts = 0;
    }
}

void CECDaemon::onReconnectRetryTimer() {
    m_reconnectRetryTimer.consume();
    if (!m_router) return;
    if (m_router->isSuspended() || m_router->isAdapterValid()) {
        // Either the adapter is already back (a concurrent retry won,
        // or resume() reopened it) or we are now suspended. Reset the
        // cycle either way.
        m_reconnectAttempts = 0;
        return;
    }
    LOG_INFO("Performing retried CEC reconnection (attempt ",
             m_reconnectAttempts + 1, "/",
             kConnectionLostRetrySchedule.size() + 1, ")");
    submitReconnectAttempt();
}

bool CECDaemon::setupPowerMonitor() {
    LOG_INFO("Setting up D-Bus power monitoring");
    try {
        m_dbusMonitor = std::make_unique<DBusMonitor>();
        if (!m_dbusMonitor->initialize()) {
            LOG_ERROR("Failed to initialize D-Bus monitor");
            m_dbusMonitor.reset();
            return false;
        }
        m_dbusMonitor->setCallback([this](DBusMonitor::PowerState state) {
            // Runs inline inside sd_bus_process; hop through m_work so the
            // sequencer never executes nested in the bus dispatch stack.
            const auto event = (state == DBusMonitor::PowerState::Suspending)
                               ? PowerEvent::Suspend
                               : PowerEvent::Resume;
            m_work.post([this, event]() {
                this->enqueuePowerEvent(event, EventSource::DBus);
            });
        });
        LOG_INFO("D-Bus power monitoring setup successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during D-Bus power monitor setup: ", e.what());
        m_dbusMonitor.reset();
        return false;
    }
}

Message CECDaemon::handleCommand(const Message& command) {
    LOG_DEBUG("Received command: type=", static_cast<int>(command.type),
              ", deviceId=", static_cast<int>(command.deviceId));

    // handleCommand runs on a socket-connection pool worker, not the main
    // thread. Suspend/resume mutate main-thread-only state (timer arming,
    // coordination flags) and must therefore be dispatched via the work
    // queue. Acknowledge the request as soon as it's been queued.
    if (command.type == MessageType::CMD_SUSPEND) {
        LOG_INFO("Processing suspend command");
        m_work.post([this]() {
            this->enqueuePowerEvent(PowerEvent::Suspend, EventSource::Wire);
        });
        return Message(MessageType::RESP_SUCCESS);
    }
    if (command.type == MessageType::CMD_RESUME) {
        LOG_INFO("Processing resume command");
        m_work.post([this]() {
            this->enqueuePowerEvent(PowerEvent::Resume, EventSource::Wire);
        });
        return Message(MessageType::RESP_SUCCESS);
    }

    if (!m_router) {
        LOG_ERROR("Command router not initialized");
        return Message(MessageType::RESP_ERROR);
    }

    return m_router->dispatch(command);
}

} // namespace cec_control
