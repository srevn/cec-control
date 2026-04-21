#pragma once

#include <chrono>
#include <deque>
#include <memory>

#include "../common/backoff_schedule.h"
#include "../common/event_loop.h"
#include "../common/main_thread_work.h"
#include "../common/signal_source.h"
#include "../common/timer_source.h"
#include "command_router.h"
#include "dbus_monitor.h"
#include "power/adapter_reconnect.h"
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
     * Power-lifecycle sequencer phases. Exactly one lifecycle event is in
     * flight at a time; subsequent events queue in m_pendingEvents until
     * the current phase returns to Idle.
     */
    enum class LifecyclePhase { Idle, Suspending, Resuming };

    /** Events fed into the sequencer. */
    enum class PowerEvent { Suspend, Resume };

    /**
     * Where a PowerEvent originated. D-Bus events are bound to a real
     * logind sleep cycle and must release/retake the delay-inhibitor
     * lock. Wire events are local operator actions and leave the lock
     * alone.
     */
    enum class EventSource { DBus, Wire };

    struct PendingPowerEvent {
        PowerEvent  event;
        EventSource source;
    };

    /**
     * Enqueue a lifecycle event on the main thread. Safe to call inline
     * from any main-thread context (dbus callback, m_work drain). Do not
     * call from worker threads directly; use m_work.post() to hop the
     * main thread first.
     */
    void enqueuePowerEvent(PowerEvent event, EventSource source);

    /**
     * Drain the next queued event if the sequencer is Idle. Invoked on
     * every phase-to-Idle transition and on every fresh enqueue.
     */
    void startNextLifecycleEvent();

    /** Begin a suspend cycle: arm safety timer, submit router->suspend() to the pool. */
    void startSuspend(EventSource source);

    /** Begin a resume cycle: submit router->resume() to the pool. */
    void startResume(EventSource source);

    /**
     * Main-thread continuation posted from the suspend pool worker.
     * Releases the inhibit lock (iff source was D-Bus), transitions the
     * phase back to Idle, and drains the next queued event.
     */
    void onSuspendComplete(std::chrono::milliseconds workDuration);

    /**
     * Main-thread continuation posted from the resume pool worker. Arms
     * a delayed retry if the adapter failed to come back, retakes the
     * inhibit lock (iff source was D-Bus), then drains the next event.
     */
    void onResumeComplete(bool adapterValid);

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
     * timer or give up per m_reconnectSchedule.
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

    // Suspend / resume coordination state. Main thread only; no atomics.
    LifecyclePhase                m_phase = LifecyclePhase::Idle;
    EventSource                   m_phaseSource = EventSource::DBus;
    std::deque<PendingPowerEvent> m_pendingEvents;
    bool m_suspendSafetyArmed   = false;
    bool m_resumeRetryPending   = false;
    int  m_terminationSignalCount = 0;

    // Connection-lost reconnect state machine. The first attempt fires
    // immediately from the ConnectionLost event; entries below are the
    // delays between subsequent retries. Reset on success, on give-up,
    // and on any suspend/resume transition. Main thread only.
    AdapterReconnect m_adapterReconnect{BackoffSchedule{
        std::chrono::seconds(5),
        std::chrono::seconds(10),
        std::chrono::seconds(20),
    }};

    // Daemon-level lifecycle options.
    Options m_options;

    // True between start() returning success and stop() completing.
    bool m_started = false;
};

} // namespace cec_control
