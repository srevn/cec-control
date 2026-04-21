#include "cec_daemon.h"
#include "../common/logger.h"
#include "../common/systemd_env.h"

#include <chrono>
#include <csignal>
#include <utility>

namespace cec_control {

CECDaemon::CECDaemon(Options daemonOptions, CommandRouter::Options routerOptions)
    : m_signals{SIGINT, SIGTERM, SIGHUP},
      m_taskPool(std::make_shared<ThreadPool>(2)),
      m_connectionPool(std::make_shared<ThreadPool>(SocketServer::kMaxConnections)),
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
    // the SignalSource member's constructor. Start the pools after masking
    // and before spawning any other threads so SIGINT/SIGTERM/SIGHUP are
    // only delivered to us via signalfd on the main thread.
    m_taskPool->start();
    m_connectionPool->start();

    try {
        // Build the router's outbound hooks before construction. Both land
        // on the main thread via m_work so lifecycle state (FSM, timers)
        // is only ever touched from one thread, regardless of which
        // libCEC- or pool-owned thread fires the event.
        CommandRouter::Callbacks routerCallbacks{
            /*onConnectionLost*/ [this]() {
                m_work.post([this]() { this->onConnectionLost(); });
            },
            /*onSuspendRequested*/ [this]() {
                // Router fires this on a pool worker. sd-bus is
                // single-owner (main thread), so hop there; the actual
                // Suspend() success/failure is logged by DBusMonitor,
                // so this side is fire-and-forget.
                m_work.post([this]() {
                    if (m_dbusMonitor) m_dbusMonitor->suspendSystem();
                });
            },
        };

        m_router = std::make_unique<CommandRouter>(
            std::move(m_routerOptions), m_taskPool, std::move(routerCallbacks));

        if (!m_router->initialize()) {
            LOG_ERROR("Failed to initialize command router");
            return false;
        }

        m_socketServer = std::make_unique<SocketServer>(m_connectionPool);
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
                LOG_WARNING("Failed to set up power monitoring. "
                            "Sleep/wake events will not be handled automatically.");
            }
        } else {
            LOG_INFO("D-Bus power monitoring disabled via configuration. "
                     "Suspend/resume operations will require manual commands.");
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

    // Ordered teardown. The invariant is that no pool worker ever observes
    // a destroyed subsystem: we therefore drain pools *before* releasing
    // the unique_ptrs whose members the captured lambdas reach into.
    //
    //   1. detach DBusMonitor so no further bus events reach us
    //   2. stop accepting connections and drain in-flight handlers
    //   3. join the connection pool; from here no socket worker can
    //      submit anything new to the task pool
    //   4. soft-close the router (m_shutdownComplete = true, adapter
    //      closed, queue cleared) while it is still alive
    //   5. drain the task pool: any queued suspend/resume/restart
    //      lambda observes m_shutdownComplete and becomes a no-op
    //   6. stop DBusMonitor (bus was detached at step 1)
    //   7. destroy subsystems and pools in dependency order
    try {
        // Each subsystem logs its own "Stopping" / "Stopped" lifecycle; the
        // daemon adds wall-clock timing so slow teardowns surface at a
        // glance without double-logging the transitions themselves.
        if (m_dbusMonitor) {
            m_dbusMonitor->detach();
        }

        if (m_socketServer) {
            const auto t0 = std::chrono::steady_clock::now();
            m_socketServer->stop();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Socket server teardown: ", ms, "ms");
        }

        if (m_connectionPool) {
            m_connectionPool->shutdown();
        }

        if (m_router) {
            const auto t0 = std::chrono::steady_clock::now();
            m_router->shutdown();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Command router teardown: ", ms, "ms");
        }

        if (m_taskPool) {
            m_taskPool->shutdown();
        }

        if (m_dbusMonitor) {
            m_dbusMonitor->stop();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon shutdown: ", e.what());
    }

    // No thread remains that can touch any subsystem. Destroy in reverse
    // dependency order: domain subsystems first, then the pools that
    // ultimately ran their closures.
    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_router.reset();
    m_connectionPool.reset();
    m_taskPool.reset();

    LOG_INFO("Shutdown sequence complete");
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

void CECDaemon::applyLifecycle(PowerLifecycle::Output output) {
    executeEffects(output);
    executeEffects(m_lifecycle.pumpQueue());
}

void CECDaemon::executeEffects(const PowerLifecycle::Output& out) {
    using Timer = PowerLifecycle::Output::Timer;
    using Work  = PowerLifecycle::Output::Work;
    using Lock  = PowerLifecycle::Output::Lock;
    using Notify = PowerLifecycle::Output::Notify;

    // Entry banner for a new cycle. Emitted before any other effect so
    // a concurrent timer-arm warning lands *after* "starting X" in the log.
    switch (out.work) {
    case Work::StartSuspend:
        LOG_INFO("System suspending, preparing CEC adapter");
        break;
    case Work::StartResume:
        LOG_INFO("System resuming, reinitializing CEC adapter");
        break;
    case Work::None:
        break;
    }

    // Disarm stale timers first so a subsequent Arm on the same axis
    // starts from a clean slate. TimerSource::disarm is idempotent.
    if (out.safetyTimer == Timer::Disarm) m_suspendSafetyTimer.disarm();
    if (out.resumeRetry == Timer::Disarm) m_resumeRetryTimer.disarm();

    // Notify the reconnect FSM. Its effects are carried out inline by
    // execute(); a SystemSuspend/SystemResume there disarms the
    // reconnect retry timer if one was armed.
    switch (out.reconnectNotify) {
    case Notify::SystemSuspend:
        execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::SystemSuspend));
        break;
    case Notify::SystemResume:
        execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::SystemResume));
        break;
    case Notify::None:
        break;
    }

    // Arm new timers. On syscall failure, feed back to the FSM so its
    // internal armed flags stay accurate; the returned Output is always
    // inert for these feedback events, so no further dispatch is needed.
    if (out.safetyTimer == Timer::Arm) {
        if (!m_suspendSafetyTimer.armOnce(out.safetyDelay)) {
            LOG_WARNING("Could not arm suspend-safety timer; "
                        "inhibit lock will only release on completion");
            (void)m_lifecycle.onSafetyTimerArmFailed();
        }
    }
    if (out.resumeRetry == Timer::Arm) {
        if (!m_resumeRetryTimer.armOnce(out.resumeRetryDelay)) {
            LOG_WARNING("Could not arm resume-retry timer");
            (void)m_lifecycle.onResumeRetryArmFailed();
        }
    }

    // Inhibit-lock operations. m_dbusMonitor may be absent when power
    // monitoring is disabled by configuration or failed to initialise;
    // lock ops are no-ops in that case.
    if (m_dbusMonitor) {
        switch (out.lock) {
        case Lock::Release: m_dbusMonitor->releaseInhibitLock(); break;
        case Lock::Take:    m_dbusMonitor->takeInhibitLock();    break;
        case Lock::None:    break;
        }
    }

    // Pool-side work.
    switch (out.work) {
    case Work::StartSuspend: submitSuspendWork(); break;
    case Work::StartResume:  submitResumeWork();  break;
    case Work::None:         break;
    }
    if (out.submitLateReconnect) submitLateReconnect();
}

void CECDaemon::submitSuspendWork() {
    if (!m_router) {
        // Defensive: daemon mid-teardown. Post a synthetic completion
        // via m_work so the FSM keeps draining on a subsequent loop
        // iteration rather than re-entering applyLifecycle inline.
        m_work.post([this]() {
            this->onSuspendComplete(std::chrono::milliseconds(0));
        });
        return;
    }
    // Run the CEC-side suspend on a pool worker so the main loop continues
    // to service signals, sockets, and DBus while we wait.
    m_taskPool->submit([this]() {
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

void CECDaemon::submitResumeWork() {
    if (!m_router) {
        m_work.post([this]() { this->onResumeComplete(false); });
        return;
    }
    m_taskPool->submit([this]() {
        try {
            m_router->resume();
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during resume: ", e.what());
        }
        const bool adapterValid = m_router->isAdapterValid();
        m_work.post([this, adapterValid]() {
            this->onResumeComplete(adapterValid);
        });
    });
}

void CECDaemon::submitLateReconnect() {
    m_taskPool->submit([this]() {
        if (!m_router) return;
        if (m_router->isAdapterValid() || m_router->isSuspended()) return;
        LOG_INFO("Performing delayed reconnection attempt");
        (void)m_router->reconnect();
    });
}

void CECDaemon::onSuspendComplete(std::chrono::milliseconds workDuration) {
    // Whichever path (completion vs. safety timer) fires first discharges
    // the inhibit-lock release; the other path takes the overrun branch.
    auto out = m_lifecycle.onSuspendCompleted();
    if (out.safetyOverrun) {
        LOG_WARNING("CEC suspend completed in ", workDuration.count(),
                    "ms but safety timer had already released the inhibit lock");
    } else {
        LOG_INFO("CEC suspend took ", workDuration.count(), "ms");
        if (out.lock == PowerLifecycle::Output::Lock::Release) {
            LOG_INFO("CEC sleep preparation complete; allowing system to sleep");
        }
    }
    applyLifecycle(out);
}

void CECDaemon::onResumeComplete(bool adapterValid) {
    // After resume, the USB subsystem can still be settling. The FSM
    // emits a Timer::Arm for one delayed retry (gated on !adapterValid
    // and not-already-armed); the lock retake happens for DBus sources.
    applyLifecycle(m_lifecycle.onResumeCompleted(adapterValid));
}

void CECDaemon::onSuspendSafetyTimer() {
    m_suspendSafetyTimer.consume();
    auto out = m_lifecycle.onSafetyTimerFired();
    if (out.safetyFired) {
        LOG_WARNING("Suspend did not complete within ",
                    PowerLifecycle::kSuspendSafetyDeadline.count(),
                    "s; releasing inhibit lock forcibly");
    }
    applyLifecycle(out);
}

void CECDaemon::onResumeRetryTimer() {
    m_resumeRetryTimer.consume();
    applyLifecycle(m_lifecycle.onResumeRetryTimerFired());
}

void CECDaemon::onConnectionLost() {
    LOG_WARNING("CEC connection lost, attempting to reconnect");
    execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::ConnectionLost));
}

void CECDaemon::submitReconnectAttempt() {
    m_taskPool->submit([this]() {
        const bool ok = m_router && m_router->reconnect();
        m_work.post([this, ok]() { this->onReconnectResult(ok); });
    });
}

void CECDaemon::onReconnectResult(bool ok) {
    // A result arriving after a SystemSuspend/Resume has already moved
    // the FSM to Idle is absorbed as a no-op; no state check needed.
    if (ok) LOG_INFO("Successfully reconnected to CEC adapter");
    execute(m_adapterReconnect.onEvent(
        ok ? AdapterReconnect::Event::AttemptSucceeded
           : AdapterReconnect::Event::AttemptFailed));
}

void CECDaemon::onReconnectRetryTimer() {
    m_reconnectRetryTimer.consume();
    execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::RetryTimerFired));
}

void CECDaemon::execute(AdapterReconnect::Output out) {
    using E = AdapterReconnect::Effect;
    switch (out.effect) {
    case E::None:
        break;
    case E::StartAttempt:
        // StartAttempt supersedes any armed retry timer; disarm()
        // unconditionally so the dispatcher stays free of state checks.
        m_reconnectRetryTimer.disarm();
        if (out.attemptNumber > 1) {
            LOG_INFO("Performing retried CEC reconnection (attempt ",
                     out.attemptNumber, "/", out.totalAttempts, ")");
        }
        submitReconnectAttempt();
        break;
    case E::ScheduleRetry:
        LOG_WARNING("CEC reconnect attempt failed; next retry in ",
                    out.delay.count(), "ms (attempt ",
                    out.attemptNumber, "/", out.totalAttempts, ")");
        if (!m_reconnectRetryTimer.armOnce(out.delay)) {
            LOG_ERROR("Failed to arm reconnect-retry timer");
            execute(m_adapterReconnect.onEvent(
                AdapterReconnect::Event::TimerArmFailed));
        }
        break;
    case E::CancelRetry:
        m_reconnectRetryTimer.disarm();
        break;
    case E::AbandonCycle:
        LOG_WARNING("CEC reconnect abandoned after ", out.totalAttempts,
                    " attempts; waiting for next connection-lost event");
        break;
    }
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
            m_work.post([this, state]() {
                auto out = (state == DBusMonitor::PowerState::Suspending)
                    ? m_lifecycle.onSuspendRequested(PowerLifecycle::Source::DBus)
                    : m_lifecycle.onResumeRequested(PowerLifecycle::Source::DBus);
                this->applyLifecycle(out);
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
    // thread. Suspend/resume mutate main-thread-only FSM state and must
    // therefore be dispatched via the work queue. Acknowledge the request
    // as soon as it's been queued.
    if (command.type == MessageType::CMD_SUSPEND) {
        LOG_INFO("Processing suspend command");
        m_work.post([this]() {
            this->applyLifecycle(
                m_lifecycle.onSuspendRequested(PowerLifecycle::Source::Wire));
        });
        return Message(MessageType::RESP_SUCCESS);
    }
    if (command.type == MessageType::CMD_RESUME) {
        LOG_INFO("Processing resume command");
        m_work.post([this]() {
            this->applyLifecycle(
                m_lifecycle.onResumeRequested(PowerLifecycle::Source::Wire));
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
