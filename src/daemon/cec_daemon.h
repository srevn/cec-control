#pragma once

#include <chrono>
#include <memory>

#include "../common/backoff_schedule.h"
#include "../common/event_loop.h"
#include "../common/main_thread_work.h"
#include "../common/signal_source.h"
#include "../common/timer_source.h"
#include "command_router.h"
#include "dbus_monitor.h"
#include "power/adapter_reconnect.h"
#include "power/power_lifecycle.h"
#include "socket_server.h"
#include "thread_pool.h"

namespace cec_control {

/**
 * @class CECDaemon
 * @brief Top-level orchestrator: owns the unified event loop and wires
 *        every event source (signals, work queue, sockets, DBus, timers)
 *        into it. All long-running work is delegated to the thread pool
 *        so the main loop itself stays responsive.
 *
 * After Phase E the daemon carries no singleton, no C-style signal
 * handler, and no cross-thread atomic flags. Shutdown is triggered by
 * SignalSource, which calls m_loop.stop(); teardown runs in stop() on
 * return from run(). A third SIGTERM during a stuck teardown does not
 * self-escalate to _exit — systemd's TimeoutStopSec plus SIGKILL is
 * the operator escape hatch.
 */
class CECDaemon {
public:
    /**
     * Daemon-level lifecycle knobs. CEC-specific options live on
     * CommandRouter::Options.
     */
    struct Options {
        bool enablePowerMonitor = true;
    };

    CECDaemon(Options daemonOptions, CommandRouter::Options routerOptions);
    ~CECDaemon();

    CECDaemon(const CECDaemon&) = delete;
    CECDaemon& operator=(const CECDaemon&) = delete;

    /** Bring up pool, router, socket server, DBus monitor, loop sources. */
    [[nodiscard]] bool start();

    /** Block on the unified event loop until a signal or error exits it. */
    void run();

    /** Tear everything down. Idempotent; safe to call after a failed start(). */
    void stop();

private:
    /**
     * Main-thread continuation posted from the suspend pool worker.
     * Feeds the lifecycle FSM; on a DBus-sourced cycle the emitted
     * Lock::Release drops the inhibit lock, unless the safety timer
     * already claimed that obligation.
     */
    void onSuspendComplete(std::chrono::milliseconds workDuration);

    /**
     * Main-thread continuation posted from the resume pool worker.
     * Feeds the lifecycle FSM; the emitted Timer::Arm covers the
     * post-resume USB re-enumeration window, and on a DBus-sourced
     * cycle the emitted Lock::Take re-installs the inhibit lock.
     */
    void onResumeComplete(bool adapterValid);

    /** Handler for signalfd readability. */
    void onSignalReadable();

    /** Handler for the suspend-safety timer. */
    void onSuspendSafetyTimer();

    /** Handler for the post-resume delayed-retry timer. */
    void onResumeRetryTimer();

    /**
     * Main-thread entry point for a libCEC connection-lost alert. Feeds
     * the reconnect FSM and kicks off the first attempt.
     */
    void onConnectionLost();

    /**
     * Run a single reconnect attempt on the thread pool. Results land
     * back on the main loop via MainThreadWork → onReconnectResult.
     */
    void submitReconnectAttempt();

    /**
     * Process the outcome of the most recent reconnect attempt: feed
     * AttemptSucceeded or AttemptFailed to the reconnect FSM.
     */
    void onReconnectResult(bool ok);

    /**
     * Handler for the connection-lost retry timer: invoked between
     * attempts. Feeds the FSM; any staleness (suspend/resume already
     * closed the cycle) is absorbed as a no-op by the FSM's Idle state.
     */
    void onReconnectRetryTimer();

    /** Carry out the side-effect emitted by the reconnect FSM. */
    void execute(AdapterReconnect::Output out);

    /**
     * Apply an Output emitted by the lifecycle FSM, then drain the next
     * queued event (if any) via PowerLifecycle::pumpQueue. A single
     * post-event pump suffices because a successful pump leaves the
     * FSM non-Idle, blocking further pumps.
     */
    void applyLifecycle(PowerLifecycle::Output output);

    /**
     * Flat switch over every axis in the Output. Execution order is
     * chosen for reviewability: disarm stale timers first, notify the
     * reconnect FSM, arm new timers (with ArmFailed feedback on
     * syscall failure), apply lock ops, then submit pool-side work.
     */
    void executeEffects(const PowerLifecycle::Output& output);

    /** Submit router->suspend() to the task pool with timing. */
    void submitSuspendWork();

    /** Submit router->resume() to the task pool. */
    void submitResumeWork();

    /**
     * Fire-and-forget reconnect attempt triggered by the post-resume
     * retry timer. Separate from the AdapterReconnect cycle: covers
     * the specific USB-reenumeration race after wake.
     */
    void submitLateReconnect();

    /** Route an incoming wire command to the right subsystem. */
    Message handleCommand(const Message& command);

    /** Ensure a DBus monitor is up; returns true on success, false on
     *  initialization failure (logged). */
    [[nodiscard]] bool setupPowerMonitor();

    // Event loop and single-threaded primitives. Declared first so they
    // outlive every subsystem that might register handlers against them.
    // SignalSource must be constructed on the main thread before any worker
    // threads are spawned — it masks the relevant signals on the current
    // thread, and workers inherit the mask via thread creation.
    SignalSource      m_signals;
    MainThreadWork    m_work;
    EventLoop         m_loop;
    TimerSource       m_suspendSafetyTimer;
    TimerSource       m_resumeRetryTimer;
    TimerSource       m_reconnectRetryTimer;

    // Cross-thread worker pools. Must outlive router and socket server.
    //
    // Split by purpose so connection handlers (up to
    // SocketServer::kMaxConnections long-lived recv loops) never starve
    // lifecycle tasks (suspend, resume, reconnect, adapter restart).
    // The task pool is intentionally small: the router mutex serialises
    // every lifecycle task, so one worker would suffice; two gives
    // headroom against a rare pathology (e.g. libcec wedged inside
    // reopen) monopolising the queue.
    std::shared_ptr<ThreadPool> m_taskPool;
    std::shared_ptr<ThreadPool> m_connectionPool;

    // Domain subsystems.
    std::unique_ptr<CommandRouter>  m_router;
    std::unique_ptr<SocketServer>   m_socketServer;
    std::unique_ptr<DBusMonitor>    m_dbusMonitor;

    // Staged options. Router consumes them on initialize().
    CommandRouter::Options m_routerOptions;

    // Pure decision types driving suspend/resume arbitration and the
    // connection-lost reconnect cycle. Both main-thread only; no
    // atomics or mutexes. Side effects are carried out by the daemon's
    // fd handlers and pool submissions.
    PowerLifecycle m_lifecycle;
    AdapterReconnect m_adapterReconnect{BackoffSchedule{
        std::chrono::seconds(5),
        std::chrono::seconds(10),
        std::chrono::seconds(20),
    }};

    // Daemon-level lifecycle options.
    Options m_options;

    int  m_terminationSignalCount = 0;

    // True between start() returning success and stop() completing.
    bool m_started = false;
};

} // namespace cec_control
