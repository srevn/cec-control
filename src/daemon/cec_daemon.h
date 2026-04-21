#pragma once

#include <chrono>
#include <memory>

#include "../common/backoff_schedule.h"
#include "../common/event_loop.h"
#include "../common/main_thread_work.h"
#include "../common/signal_source.h"
#include "../common/timer_source.h"
#include "cec/adapter_worker.h"
#include "cec/libcec_adapter.h"
#include "command_router.h"
#include "dbus_monitor.h"
#include "power/adapter_reconnect.h"
#include "power/power_lifecycle.h"
#include "socket_server.h"

namespace cec_control {

/**
 * @class CECDaemon
 * @brief Top-level orchestrator: owns the unified event loop and wires
 *        every event source (signals, work queue, sockets, DBus,
 *        timers) into it. Blocking CEC work runs on a dedicated
 *        @c AdapterWorker thread so the main loop stays responsive.
 *
 * The daemon owns the adapter lifecycle: it constructs the
 * @c LibCecAdapter on the main thread, transfers it to
 * @c AdapterWorker, then builds the @c CommandRouter with a reference
 * to both the worker and the main-thread work queue. libcec's
 * internal threads are spawned only once the worker is observable —
 * @c openConnection is submitted as the worker's first job — so
 * callback forwarders always see a fully-wired daemon.
 *
 * Shutdown drives a strict ordering so no thread observes a destroyed
 * subsystem: the socket server stops before the router's shutdown
 * flag is flipped, the worker stops (closing the adapter on its
 * exit path) before subsystems are released, and destruction runs in
 * reverse construction order.
 */
class CECDaemon {
public:
    /**
     * Daemon-level knobs: what the daemon process enables plus the
     * adapter configuration it owns directly. Command-router tunables
     * stay on @c CommandRouter::Options; this split mirrors the fact
     * that the daemon constructs the adapter now that it owns the
     * worker.
     */
    struct Options {
        bool                   enablePowerMonitor   = true;
        bool                   scanDevicesAtStartup = false;
        LibCecAdapter::Options adapter;
    };

    CECDaemon(Options daemonOptions, CommandRouter::Options routerOptions);
    ~CECDaemon();

    CECDaemon(const CECDaemon&)            = delete;
    CECDaemon& operator=(const CECDaemon&) = delete;

    /**
     * Construct and initialise the adapter, hand it to the worker,
     * build the router, register every event-loop source, and submit
     * @c openConnection as the worker's first job. Returns @c true on
     * success; a failure path leaves the daemon in a state where
     * @c stop() will tear down whatever partial state was committed.
     */
    [[nodiscard]] bool start();

    /** Block on the event loop until a signal or error exits it. */
    void run();

    /** Tear everything down. Idempotent; safe after a failed @c start(). */
    void stop();

private:
    /**
     * Continuation fired by @c CommandRouter::suspendAsync on the main
     * thread. Feeds the lifecycle FSM; on a DBus-sourced cycle the
     * emitted @c Lock::Release drops the inhibit lock unless the
     * safety timer already claimed that obligation.
     */
    void onSuspendComplete(std::chrono::milliseconds workDuration);

    /**
     * Continuation fired by @c CommandRouter::resumeAsync on the main
     * thread. Feeds the lifecycle FSM; the emitted @c Timer::Arm
     * covers the post-resume USB re-enumeration window, and on a
     * DBus-sourced cycle the emitted @c Lock::Take re-installs the
     * inhibit lock.
     */
    void onResumeComplete(bool adapterValid);

    /** Handler for signalfd readability. */
    void onSignalReadable();

    /** Handler for the suspend-safety timer. */
    void onSuspendSafetyTimer();

    /** Handler for the post-resume delayed-retry timer. */
    void onResumeRetryTimer();

    /**
     * Main-thread entry point for a libcec connection-lost alert.
     * Feeds the reconnect FSM and kicks off the first attempt.
     */
    void onConnectionLost();

    /** Submit one reconnect attempt; result lands in @c onReconnectResult. */
    void submitReconnectAttempt();

    /**
     * Process the outcome of the most recent reconnect attempt: feed
     * @c AttemptSucceeded or @c AttemptFailed to the reconnect FSM.
     */
    void onReconnectResult(bool ok);

    /**
     * Handler for the connection-lost retry timer: invoked between
     * attempts. Feeds the FSM; any staleness is absorbed as a no-op
     * by the FSM's @c Idle state.
     */
    void onReconnectRetryTimer();

    /** Carry out the side-effect emitted by the reconnect FSM. */
    void execute(AdapterReconnect::Output out);

    /**
     * Apply an @c Output emitted by the lifecycle FSM, then pump any
     * queued event via @c PowerLifecycle::pumpQueue. A single
     * post-event pump suffices because a successful pump leaves the
     * FSM non-Idle, blocking further pumps.
     */
    void applyLifecycle(PowerLifecycle::Output output);

    /**
     * Flat switch over every axis in the @c Output. Execution order is
     * chosen for reviewability: disarm stale timers first, notify the
     * reconnect FSM, arm new timers (with @c ArmFailed feedback on
     * syscall failure), apply lock ops, then kick off adapter work.
     */
    void executeEffects(const PowerLifecycle::Output& output);

    /** Kick off @c router->suspendAsync with timing and completion wiring. */
    void submitSuspendWork();

    /** Kick off @c router->resumeAsync with completion wiring. */
    void submitResumeWork();

    /**
     * Fire-and-forget reconnect attempt triggered by the post-resume
     * retry timer. Separate from the @c AdapterReconnect cycle: covers
     * the specific USB re-enumeration race after wake.
     */
    void submitLateReconnect();

    /**
     * Route an incoming wire command. Runs on the main thread.
     * Suspend and resume feed the lifecycle FSM synchronously and
     * reply inline; everything else is forwarded to the router whose
     * @c dispatch chooses between an inline reply and a worker hop.
     */
    void handleCommand(Message command, SocketServer::ResponseSink reply);

    /**
     * Adapter callback forwarder: TV standby. Fires on libcec's
     * command thread; delegates to the router (which reads an atomic
     * and, if enabled, fires the suspend-request callback). The
     * daemon owns the forwarder rather than wiring libcec directly to
     * the router so that the callback target is stable across
     * router/adapter construction order.
     */
    void onAdapterTvStandby();

    /**
     * Adapter callback forwarder: connection lost. Fires on libcec's
     * alert thread; hops the observation through @c m_work so the
     * reconnect FSM transition runs on the main thread.
     */
    void onAdapterConnectionLost();

    /** Ensure a DBus monitor is up; returns @c true on success. */
    [[nodiscard]] bool setupPowerMonitor();

    // Event loop and single-threaded primitives. Declared first so
    // they outlive every subsystem that might register handlers
    // against them. SignalSource must be constructed on the main
    // thread before any worker threads are spawned — it masks the
    // relevant signals on the current thread, and workers inherit the
    // mask via thread creation.
    SignalSource m_signals;
    MainThreadWork m_work;
    EventLoop      m_loop;
    TimerSource    m_suspendSafetyTimer;
    TimerSource    m_resumeRetryTimer;
    TimerSource    m_reconnectRetryTimer;

    // CEC adapter actor. Owns the libcec handle and the single thread
    // that is allowed to touch it. Built before the router so the
    // router can capture a reference; the adapter itself is handed
    // into the worker at construction. Destroyed in @c stop(), which
    // joins the worker thread and closes the adapter on its exit path.
    std::unique_ptr<AdapterWorker> m_worker;

    // Domain subsystems.
    std::unique_ptr<CommandRouter> m_router;
    std::unique_ptr<SocketServer>  m_socketServer;
    std::unique_ptr<DBusMonitor>   m_dbusMonitor;

    // Staged options. Router consumes them at construction in start();
    // adapter options are read out of m_options before the adapter is
    // built.
    CommandRouter::Options m_routerOptions;

    // Pure decision types driving suspend/resume arbitration and the
    // connection-lost reconnect cycle. Both main-thread only; no
    // atomics or mutexes. Side effects are carried out by the daemon's
    // fd handlers and worker submissions.
    PowerLifecycle   m_lifecycle;
    AdapterReconnect m_adapterReconnect{BackoffSchedule{
        std::chrono::seconds(5),
        std::chrono::seconds(10),
        std::chrono::seconds(20),
    }};

    // Daemon-level lifecycle options.
    Options m_options;

    int m_terminationSignalCount = 0;

    // True between start() returning success and stop() completing.
    bool m_started = false;
};

} // namespace cec_control
