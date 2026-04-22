#include "cec_daemon.h"

#include <chrono>
#include <csignal>
#include <utility>

#include "../common/logger.h"
#include "../common/system_paths.h"
#include "../common/systemd_env.h"
#include "cec/operations.h"

namespace cec_control {

CECDaemon::CECDaemon(AppConfig config)
    : m_signals{SIGINT, SIGTERM, SIGHUP},
      m_config(std::move(config)) {}

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

    try {
        // Build the adapter on the main thread. Callbacks target
        // daemon forwarders so the wiring does not depend on the
        // router's construction order, and so libcec-thread entry
        // points are stable for the lifetime of the adapter.
        ICecAdapter::Callbacks adapterCallbacks{
            /*onTvStandby*/      [this]() { this->onAdapterTvStandby(); },
            /*onConnectionLost*/ [this]() { this->onAdapterConnectionLost(); },
        };
        // Copy (not move) the adapter config: the daemon keeps
        // m_config intact for a future SIGHUP reload diff, and the
        // sub-struct is small enough that the copy is noise.
        auto adapter = std::make_unique<LibCecAdapter>(
            m_config.adapter, std::move(adapterCallbacks));

        // initialize() loads libcec and detects adapter hardware. It
        // does NOT spawn libcec's command or alert threads — those
        // start inside openConnection() below.
        if (!adapter->initialize()) {
            LOG_ERROR("Failed to initialize CEC adapter library");
            return false;
        }

        // Open the adapter on the main thread. libcec's Open is
        // thread-identity-agnostic: the internal command and alert
        // threads it spawns reference only the stable address of the
        // callback struct embedded in the adapter, not the calling
        // thread's identity. Doing it here keeps the startup path
        // linear — no promise/future round trip, no worker submit
        // before the worker is even spawned. From m_worker->start()
        // onwards the worker is the sole thread that invokes any
        // other libcec method, and its close-on-exit handles teardown.
        if (!adapter->openConnection()) {
            LOG_ERROR("Failed to open CEC adapter connection");
            return false;
        }

        m_worker = std::make_unique<AdapterWorker>(std::move(adapter));

        // Router's only outbound hook is the TV-standby-driven system
        // suspend. Fires on libcec's command thread (via the daemon
        // forwarder) so it MUST hop through m_work: sd-bus is
        // main-thread owned.
        CommandRouter::Callbacks routerCallbacks{
            /*onSuspendRequested*/ [this]() {
                m_work.post([this]() {
                    if (m_dbusMonitor) m_dbusMonitor->suspendSystem();
                });
            },
        };
        m_router = std::make_unique<CommandRouter>(
            m_config, *m_worker, m_work, std::move(routerCallbacks));

        m_worker->start();

        if (m_config.daemon.scanDevicesAtStartup) {
            LOG_INFO("Scanning for CEC devices...");
            m_worker->submit([](ICecAdapter& adapter) {
                ops::logDeviceSnapshot(adapter);
            });
        } else {
            LOG_INFO("Skipping device scanning");
        }

        m_socketServer = std::make_unique<SocketServer>(
            m_loop, SystemPaths::getSocketPath());
        m_socketServer->setCommandHandler(
            [this](Message command, SocketServer::ResponseSink reply) {
                this->handleCommand(std::move(command), std::move(reply));
            });
        if (!m_socketServer->start()) {
            LOG_ERROR("Failed to start socket server");
            return false;
        }

        if (m_config.daemon.enablePowerMonitor) {
            if (!setupPowerMonitor()) {
                LOG_WARNING("Failed to set up power monitoring. "
                            "Sleep/wake events will not be handled automatically.");
            }
        } else {
            LOG_INFO("D-Bus power monitoring disabled via configuration. "
                     "Suspend/resume operations will require manual commands.");
        }

        // Register every fd source with the loop. Any failure here
        // leaves earlier-registered sources in place; stop() tears
        // them down cleanly via EventLoop destruction.
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

    // Ordered teardown. The invariant is that no thread ever observes
    // a destroyed subsystem:
    //
    //   1. detach DBusMonitor so no further bus events reach us
    //   2. stop the socket server: close the listener and every
    //      session fd, so no new requests enter handleCommand
    //   3. flip the router's shutdown gate and drop queued commands
    //      (main-thread only; no adapter touch — the worker owns it)
    //   4. stop the worker: finishes the in-flight job, drops
    //      pending, closes the adapter on its exit path, joins. After
    //      this returns, libcec's internal threads are quiet and
    //      cannot reach any daemon forwarder
    //   5. stop DBusMonitor (bus was detached at step 1)
    //   6. destroy subsystems in reverse construction order
    try {
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

        if (m_router) {
            m_router->shutdown();
        }

        if (m_worker) {
            const auto t0 = std::chrono::steady_clock::now();
            m_worker->stop();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("Adapter worker teardown: ", ms, "ms");
        }

        if (m_dbusMonitor) {
            m_dbusMonitor->stop();
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during daemon shutdown: ", e.what());
    }

    // Destroy in reverse construction order: domain subsystems first,
    // then the worker whose thread their continuations might have
    // posted against. The router holds an AdapterWorker&; reset it
    // before the worker so the reference is valid right up to
    // router destruction even though the router's destructor does
    // not reach through it.
    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_router.reset();
    m_worker.reset();

    LOG_INFO("Shutdown sequence complete");
}

void CECDaemon::onSignalReadable() {
    while (auto info = m_signals.readOne()) {
        const int signum = static_cast<int>(info->ssi_signo);
        LOG_INFO("Received signal ", signum);

        // First signal requests a clean shutdown. Further signals are
        // logged but the loop has already been asked to exit; a truly
        // stuck shutdown is resolved by systemd's TimeoutStopSec
        // (which ultimately delivers SIGKILL).
        m_loop.stop();
    }
}

void CECDaemon::applyLifecycle(PowerLifecycle::Output output) {
    executeEffects(output);
    executeEffects(m_lifecycle.pumpQueue());
}

void CECDaemon::executeEffects(const PowerLifecycle::Output& out) {
    using Timer  = PowerLifecycle::Output::Timer;
    using Work   = PowerLifecycle::Output::Work;
    using Lock   = PowerLifecycle::Output::Lock;
    using Notify = PowerLifecycle::Output::Notify;

    // Entry banner for a new cycle. Emitted before any other effect so
    // a concurrent timer-arm warning lands after "starting X" in the
    // log.
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
    // internal armed flags stay accurate; the returned Output is
    // always inert for these feedback events, so no further dispatch
    // is needed.
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

    // Adapter-side work.
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
    m_router->suspendAsync([this](std::chrono::milliseconds elapsed) {
        this->onSuspendComplete(elapsed);
    });
}

void CECDaemon::submitResumeWork() {
    if (!m_router) {
        m_work.post([this]() { this->onResumeComplete(false); });
        return;
    }
    m_router->resumeAsync([this](bool adapterValid) {
        this->onResumeComplete(adapterValid);
    });
}

void CECDaemon::submitLateReconnect() {
    if (!m_router || !m_worker) return;
    if (m_worker->isAdapterConnected() || m_router->isSuspended()) return;
    LOG_INFO("Performing delayed reconnection attempt");
    m_router->reconnectAsync(/*onDone*/ {});
}

void CECDaemon::onSuspendComplete(std::chrono::milliseconds workDuration) {
    // Whichever path (completion vs. safety timer) fires first
    // discharges the inhibit-lock release; the other path takes the
    // overrun branch.
    auto out = m_lifecycle.onSuspendCompleted();
    if (out.safety == PowerLifecycle::Output::SafetyOutcome::Overrun) {
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
    if (out.safety == PowerLifecycle::Output::SafetyOutcome::Fired) {
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
    if (!m_router) {
        m_work.post([this]() { this->onReconnectResult(false); });
        return;
    }
    m_router->reconnectAsync([this](bool ok) {
        this->onReconnectResult(ok);
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
            // Runs inline inside sd_bus_process; hop through m_work so
            // the sequencer never executes nested in the bus dispatch
            // stack.
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

void CECDaemon::handleCommand(Message command, SocketServer::ResponseSink reply) {
    LOG_DEBUG("Received command: type=", static_cast<int>(command.type),
              ", deviceId=", static_cast<int>(command.deviceId));

    // Suspend and resume mutate main-thread-only FSM state. We are
    // already on the main thread here (SocketServer dispatches
    // synchronously from the event loop), so feed the lifecycle FSM
    // directly and acknowledge inline.
    switch (command.type) {
    case MessageType::CMD_SUSPEND:
        LOG_INFO("Processing suspend command");
        applyLifecycle(m_lifecycle.onSuspendRequested(PowerLifecycle::Source::Wire));
        reply(Message(MessageType::RESP_SUCCESS));
        return;
    case MessageType::CMD_RESUME:
        LOG_INFO("Processing resume command");
        applyLifecycle(m_lifecycle.onResumeRequested(PowerLifecycle::Source::Wire));
        reply(Message(MessageType::RESP_SUCCESS));
        return;
    default:
        break;
    }

    if (!m_router) {
        LOG_ERROR("Command router not initialized");
        reply(Message(MessageType::RESP_ERROR));
        return;
    }

    // The router internally chooses between an inline main-thread
    // reply (gate-only, state-only paths) and a worker hop with the
    // reply posted back via m_work.
    m_router->dispatch(std::move(command), std::move(reply));
}

void CECDaemon::onAdapterTvStandby() {
    // Fires on libcec's command thread. libcec's internal threads
    // spawn inside openConnection() on the main thread before the
    // router is built; a callback arriving in the narrow window
    // between Open() returning and m_router being assigned is
    // silently dropped by the null check. The window is microseconds
    // and in practice no TV-standby event fires during startup.
    if (auto* router = m_router.get()) {
        router->onTvStandby();
    }
}

void CECDaemon::onAdapterConnectionLost() {
    // Fires on libcec's alert thread. Hop to main so the reconnect
    // FSM transition runs single-threaded.
    m_work.post([this]() { this->onConnectionLost(); });
}

} // namespace cec_control
