#pragma once

#include <cstddef>
#include <memory>

#include "../common/event_loop.h"
#include "../common/main_thread_work.h"
#include "../common/signal_source.h"
#include "../common/timer_source.h"
#include "command_router.h"
#include "dbus_monitor.h"
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
    // DBus state-change handler (runs on main thread inside sd_bus_process).
    void handlePowerStateChange(DBusMonitor::PowerState state);

    /** Pre-sleep: arm a safety timer, run router->suspend() on the pool, then
     *  release the inhibit lock on success (or on timer fire, whichever is
     *  first). */
    void onSuspend();

    /** Post-wake: run router->resume() on the pool; on failure, arm a delayed
     *  retry timer. */
    void onResume();

    /** Handler for signalfd readability. */
    void onSignalReadable();

    /** Handler for the suspend-safety timer. */
    void onSuspendSafetyTimer();

    /** Handler for the post-resume delayed-retry timer. */
    void onResumeRetryTimer();

    /**
     * Main-thread entry point for a libCEC connection-lost alert. Resets
     * the retry state machine and kicks off the first reconnect attempt.
     */
    void onConnectionLost();

    /**
     * Run a single reconnect attempt on the thread pool. Results land
     * back on the main loop via MainThreadWork → onReconnectResult.
     */
    void submitReconnectAttempt();

    /**
     * Process the outcome of the most recent reconnect attempt: on
     * success reset the counter; on failure either arm the next retry
     * timer or give up per kConnectionLostRetrySchedule.
     */
    void onReconnectResult(bool ok);

    /**
     * Handler for the connection-lost retry timer: invoked between
     * attempts. Bails out silently if suspend / resume already fixed
     * the adapter or rendered the retry moot.
     */
    void onReconnectRetryTimer();

    /** Route an incoming wire command to the right subsystem. */
    Message handleCommand(const Message& command);

    /** Ensure a DBus monitor is up; returns true on success, false on
     *  initialization failure (logged). */
    bool setupPowerMonitor();

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

    // Cross-thread worker pool. Must outlive router and socket server.
    std::shared_ptr<ThreadPool> m_threadPool;

    // Domain subsystems.
    std::unique_ptr<CommandRouter>  m_router;
    std::unique_ptr<SocketServer>   m_socketServer;
    std::unique_ptr<DBusMonitor>    m_dbusMonitor;

    // Staged options. Router consumes them on initialize().
    CommandRouter::Options m_routerOptions;

    // Suspend / resume coordination state. Main thread only; no atomics.
    bool m_suspendSafetyArmed   = false;
    bool m_resumeRetryPending   = false;
    int  m_terminationSignalCount = 0;

    // Connection-lost retry state. Counts attempts completed in the
    // current cycle; reset to 0 after success, after giving up, or on
    // any suspend/resume. Main thread only.
    std::size_t m_reconnectAttempts = 0;

    // Daemon-level lifecycle options.
    Options m_options;

    // True between start() returning success and stop() completing.
    bool m_started = false;
};

} // namespace cec_control
