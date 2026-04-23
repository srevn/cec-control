#include "cec_daemon.h"

#include <chrono>
#include <csignal>
#include <utility>

#include "../common/logger.h"
#include "../common/system_paths.h"
#include "../common/systemd_notify.h"
// Full definitions of the subsystems cec_daemon.h forward-declares.
// Placed here so this TU can construct, call into, and destroy each
// one; cec_daemon.h stays thin and does not transitively pull libcec
// or sd-bus through these headers.
#include "adapter_lifecycle.h"
#include "cec/adapter_worker.h"
#include "cec/libcec_adapter.h"
#include "cec/operations.h"
#include "command_dispatch.h"
#include "command_dispatcher.h"
#include "dbus_monitor.h"
#include "power/power_supervisor.h"
#include "socket_server.h"
#include "standby_policy.h"

namespace cec_control {

CECDaemon::CECDaemon(AppConfig config)
    : m_signals{SIGINT, SIGTERM, SIGHUP},
      m_config(std::move(config)) {}

CECDaemon::~CECDaemon() {
    stop();
}

bool CECDaemon::start() {
    LOG_INFO("Starting CEC daemon");

    // Fail fast on a structurally broken dispatch table before any
    // subsystem that would consume it is constructed. The validator
    // logs one error per violation.
    if (!validateDispatchTable()) {
        LOG_ERROR("Dispatch table validation failed; aborting start");
        return false;
    }

    if (!m_signals.valid()) {
        LOG_ERROR("Signal source not initialised; aborting start");
        return false;
    }
    if (!m_work.valid()) {
        LOG_ERROR("Main-thread work queue not initialised; aborting start");
        return false;
    }
    if (!m_suspendSafetyTimer.valid() ||
        !m_reconnectRetryTimer.valid() ||
        !m_watchdogTimer.valid()) {
        LOG_ERROR("Timer source(s) not initialised; aborting start");
        return false;
    }

    try {
        // Build the adapter on the main thread. Callbacks target
        // daemon forwarders so the wiring does not depend on the
        // dispatcher's construction order, and so libcec-thread entry
        // points are stable for the lifetime of the adapter.
        ICecAdapter::Callbacks adapterCallbacks{
            /*onObservation*/    [this](ICecAdapter::Observation obs) {
                this->onAdapterObservation(obs);
            },
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

        // Lifecycle goes first: it owns the suspend queue and exposes
        // isSuspended/enqueue, both of which the dispatcher needs.
        m_lifecycle = std::make_unique<AdapterLifecycle>(*m_worker, m_work);

        // Standby policy: atomic flag plus an install-once suspend
        // trigger fired from libcec's command thread (via the daemon
        // forwarder). The closure hops through m_work so the actual
        // sd-bus call lands on the main thread — sd-bus is single-
        // owner. Built before the dispatcher (which holds a reference
        // to the policy); the forwarder null-guards callbacks fired
        // during the narrow window between Open() and this assignment.
        m_standbyPolicy = std::make_unique<StandbyPolicy>(
            m_config.standby.enabled,
            [this]() {
                m_work.post([this]() {
                    if (m_dbusMonitor) m_dbusMonitor->suspendSystem();
                });
            });

        m_dispatcher = std::make_unique<CommandDispatcher>(
            m_config, *m_worker, m_work, *m_lifecycle, *m_standbyPolicy);

        // Build the supervisor over the dispatcher (for replay) and
        // the lifecycle (for suspend/resume/reconnect), plus the
        // worker and timers. The dbus pointer is wired in
        // setupPowerMonitor below (or stays null when power
        // monitoring is disabled).
        //
        // The unrecoverable-adapter callback fires on the main thread
        // from the supervisor's own event handler (connection-lost
        // reconnect abandonment); requestUnrecoverableShutdown latches
        // the exit status and asks the event loop to stop so run()
        // returns and the ordered stop() sequence tears everything
        // down. Capturing @c this by value is safe: the supervisor
        // lives inside @c this, so the pointer is valid for every
        // invocation of the callback.
        m_supervisor = std::make_unique<PowerSupervisor>(
            *m_dispatcher, *m_lifecycle, *m_worker,
            m_suspendSafetyTimer, m_reconnectRetryTimer,
            [this]() { this->requestUnrecoverableShutdown(); });

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
            [this](Message command, ResponseSink reply) {
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
                        [this](uint32_t) { m_supervisor->onSafetyTimerFired(); })) {
            LOG_ERROR("Failed to register suspend-safety timer with event loop");
            return false;
        }
        if (!m_loop.add(m_reconnectRetryTimer.fd(), READ,
                        [this](uint32_t) { m_supervisor->onReconnectRetryTimerFired(); })) {
            LOG_ERROR("Failed to register reconnect-retry timer with event loop");
            return false;
        }

        if (m_dbusMonitor) {
            if (!m_dbusMonitor->attach(m_loop)) {
                LOG_WARNING("Failed to attach D-Bus monitor to event loop");
                // The monitor is about to be torn down; clear the
                // supervisor's pointer first so it cannot dangle on
                // any subsequent lock op.
                m_supervisor->setDBusMonitor(nullptr);
                m_dbusMonitor.reset();
            }
        }

        // Register the watchdog only when the unit configured one —
        // otherwise the timer stays disarmed, its fd never fires, and
        // we avoid registering a handler that would only ever no-op.
        // The service manager grants us WatchdogSec; ping at half that
        // interval so a single missed tick is not fatal.
        std::chrono::microseconds watchdogPeriod{};
        if (SystemdNotify::watchdogEnabled(watchdogPeriod)) {
            const auto pingInterval =
                std::chrono::duration_cast<std::chrono::milliseconds>(watchdogPeriod / 2);
            if (!m_loop.add(m_watchdogTimer.fd(), READ,
                            [this](uint32_t) { this->onWatchdogTimerFired(); })) {
                LOG_ERROR("Failed to register watchdog timer with event loop");
                return false;
            }
            if (!m_watchdogTimer.armPeriodic(pingInterval)) {
                LOG_ERROR("Failed to arm watchdog timer (period=",
                          pingInterval.count(), "ms); aborting start");
                return false;
            }
            LOG_INFO("Systemd watchdog active; pinging every ",
                     pingInterval.count(), "ms");
        } else {
            LOG_INFO("Systemd watchdog not configured for this unit");
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

    // Announce a clean shutdown to the service manager before any
    // subsystem teardown can begin. A no-op outside a notify-capable
    // supervisor; under systemd it distinguishes graceful stops from
    // unexpected exits for Restart= policy.
    SystemdNotify::stopping();

    // Idempotent even if run() already returned on its own.
    m_loop.stop();

    // Ordered teardown. The invariant is that no thread ever observes
    // a destroyed subsystem:
    //
    //   1. detach DBusMonitor so no further bus events reach us
    //   2. stop the socket server: close the listener and every
    //      session fd, so no new requests enter handleCommand
    //   3. flip the dispatcher's shutdown gate so any straggler call
    //      replies with RESP_ERROR (purely defensive — step 2 already
    //      cut off the wire path)
    //   4. flip the lifecycle's shutdown gate and drop queued commands
    //      (main-thread only; no adapter touch — the worker owns it)
    //   5. stop the worker: finishes the in-flight job, drops
    //      pending, closes the adapter on its exit path, joins. After
    //      this returns, libcec's internal threads are quiet and
    //      cannot reach any daemon forwarder
    //   6. stop DBusMonitor (bus was detached at step 1)
    //   7. destroy subsystems with the supervisor first so its
    //      non-owning references / pointer to dbus, dispatcher,
    //      lifecycle, worker cannot dangle. Reset the dispatcher
    //      before the lifecycle (it holds a ref to it) and both
    //      before the worker (both hold refs to it). Destroy
    //      m_standbyPolicy only after the worker has been joined
    //      so no observation closure posted by libcec's command
    //      thread can still run against a destroyed policy (the
    //      forwarder's in-closure null check is belt-and-braces).
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

        if (m_dispatcher) {
            m_dispatcher->shutdown();
        }

        if (m_lifecycle) {
            m_lifecycle->shutdown();
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

    // Reset in an order that prevents dangling references / pointer
    // dereferences. m_supervisor holds non-owning refs to m_dispatcher,
    // m_lifecycle, m_worker, m_work, the timers, plus a raw pointer to
    // m_dbusMonitor; destroying it first means none of those can be
    // touched again from supervisor code paths. m_dispatcher holds a
    // ref to m_lifecycle and to m_standbyPolicy; m_lifecycle and
    // m_dispatcher both hold refs to m_worker; m_standbyPolicy is
    // fired from libcec's command thread via the adapter forwarder
    // and must outlive the worker so the worker's join drains that
    // thread before the policy is destroyed. Chain:
    // supervisor → dispatcher → lifecycle → worker → standbyPolicy.
    m_supervisor.reset();
    m_dbusMonitor.reset();
    m_socketServer.reset();
    m_dispatcher.reset();
    m_lifecycle.reset();
    m_worker.reset();
    m_standbyPolicy.reset();

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

void CECDaemon::onWatchdogTimerFired() {
    // Drain the expiration counter so the level-triggered fd quiets
    // until the next period. We ping once per handler invocation
    // regardless of how many expirations accumulated — the service
    // manager cares about recency, not count.
    m_watchdogTimer.consume();
    SystemdNotify::watchdog();
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
            // the supervisor never executes nested in the bus dispatch
            // stack.
            m_work.post([this, state]() {
                if (state == DBusMonitor::PowerState::Suspending) {
                    m_supervisor->onSuspendRequested(PowerLifecycle::Source::DBus);
                } else {
                    m_supervisor->onResumeRequested(PowerLifecycle::Source::DBus);
                }
            });
        });
        // Hand the live monitor to the supervisor so the lifecycle
        // FSM's lock-take / lock-release effects can fire against it.
        m_supervisor->setDBusMonitor(m_dbusMonitor.get());
        LOG_INFO("D-Bus power monitoring setup successfully");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during D-Bus power monitor setup: ", e.what());
        m_dbusMonitor.reset();
        return false;
    }
}

void CECDaemon::requestUnrecoverableShutdown() {
    // Latch-on-first-fire. A second invocation is representable — a
    // manual CMD_RESTART_ADAPTER could succeed, the adapter could be
    // lost again, and the reconnect FSM could abandon a second cycle
    // all before the loop has actually returned — but the first
    // failure is the one that should propagate, and a second log line
    // would obscure the original trigger.
    if (m_exitStatus == EXIT_FAILURE) return;
    LOG_ERROR("Adapter unrecoverable; initiating daemon shutdown "
              "to allow supervisor restart");
    m_exitStatus = EXIT_FAILURE;
    m_loop.stop();
}

void CECDaemon::handleCommand(Message command, ResponseSink reply) {
    LOG_DEBUG("Received command: type=", static_cast<int>(command.type),
              ", deviceId=", static_cast<int>(command.deviceId));

    // Consult the dispatch table to decide whether this command is a
    // supervisor-intercepted lifecycle message (short-circuited here
    // into PowerSupervisor) or an ordinary wire command (forwarded to
    // CommandDispatcher). Unknown / response types fall through to the
    // dispatcher's own gate, which replies RESP_ERROR.
    const DispatchSpec* spec = findDispatchByType(command.type);
    if (spec != nullptr &&
        spec->dispatch == DispatchClass::SupervisorIntercepted) {
        // Main-thread only: SocketServer dispatches synchronously from
        // the event loop, so the supervisor mutations run inline on
        // the right thread and we can ack immediately.
        switch (command.type) {
        case MessageType::CMD_SUSPEND:
            LOG_INFO("Processing suspend command");
            m_supervisor->onSuspendRequested(PowerLifecycle::Source::Wire);
            break;
        case MessageType::CMD_RESUME:
            LOG_INFO("Processing resume command");
            m_supervisor->onResumeRequested(PowerLifecycle::Source::Wire);
            break;
        default:
            // validateDispatchTable at startup forbids a table row
            // classified SupervisorIntercepted without a case here.
            LOG_ERROR("SupervisorIntercepted command without mapping: type=",
                      static_cast<int>(command.type));
            reply(Message(MessageType::RESP_ERROR));
            return;
        }
        reply(Message(MessageType::RESP_SUCCESS));
        return;
    }

    if (!m_dispatcher) {
        LOG_ERROR("Command dispatcher not initialized");
        reply(Message(MessageType::RESP_ERROR));
        return;
    }

    // The dispatcher internally chooses between an inline main-thread
    // reply (gate / state-only paths) and a worker hop with the reply
    // posted back via m_work (AdapterCall path).
    m_dispatcher->dispatch(std::move(command), std::move(reply));
}

void CECDaemon::onAdapterObservation(ICecAdapter::Observation obs) {
    // Fires on libcec's command thread. Hop to main so subscribers
    // run single-threaded and never execute inline under a libcec
    // call. The closure captures the Observation by value — the type
    // is trivially copyable and fits in a small buffer, so there is
    // no heap allocation on the hot path beyond the usual
    // std::function machinery.
    //
    // Libcec's internal threads spawn inside openConnection() on the
    // main thread before subsystems are built; an observation
    // arriving in the narrow window between Open() returning and
    // subsystem assignment is absorbed by the in-closure null checks.
    // The window is microseconds and in practice no bus event fires
    // before construction completes.
    m_work.post([this, obs]() {
        if (auto* policy = m_standbyPolicy.get()) {
            policy->observe(obs);
        }
    });
}

void CECDaemon::onAdapterConnectionLost() {
    // Fires on libcec's alert thread. Hop to main so the supervisor's
    // reconnect FSM transition runs single-threaded.
    m_work.post([this]() {
        if (m_supervisor) m_supervisor->onConnectionLost();
    });
}

} // namespace cec_control
