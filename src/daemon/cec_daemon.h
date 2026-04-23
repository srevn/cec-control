#pragma once

#include <cstdlib>
#include <memory>

#include "../common/event_loop.h"
#include "../common/main_thread_work.h"
#include "../common/messages.h"
#include "../common/signal_source.h"
#include "../common/timer_source.h"
#include "app_config.h"
#include "cec/adapter_interface.h"

namespace cec_control {

// Daemon subsystems are held by std::unique_ptr with out-of-line
// destruction (~CECDaemon defined in cec_daemon.cpp), so forward
// declarations suffice here. The complete definitions are included
// in cec_daemon.cpp, alongside every .cpp that constructs or calls
// into them directly. Keeping those headers out of cec_daemon.h
// breaks the transitive libcec / sd-bus leak through every TU that
// just needs to name the CECDaemon type.
class AdapterLifecycle;
class AdapterWorker;
class CecHookSubsystem;
class CommandDispatcher;
class DBusMonitor;
class HookExecutor;
class PowerSupervisor;
class SocketServer;
class StandbyPolicy;

/**
 * @class CECDaemon
 * @brief Top-level wiring + bootstrap shell.
 *
 * Owns the unified event loop and the fd sources that feed it
 * (signals, main-thread work queue, three timerfds, the socket
 * listener, the sd-bus fd). Spins up every subsystem on @c start():
 * the @c AdapterWorker actor that owns the libcec handle, the
 * @c AdapterLifecycle that runs suspend / resume / reconnect over the
 * worker and parks commands during suspend, the @c StandbyPolicy
 * that owns the auto-suspend-on-TV-standby flag, the
 * @c CommandDispatcher that translates wire commands into adapter
 * calls, the @c PowerSupervisor that owns the suspend/resume and
 * adapter reconnect FSMs and bridges the lifecycle's drained queue
 * back to the dispatcher's replay path, the @c SocketServer that
 * fronts the Unix domain socket, and (optionally, gated on
 * configuration) the @c DBusMonitor that observes logind
 * PrepareForSleep.
 *
 * The daemon does NOT carry any FSM state of its own. All
 * lifecycle / reconnect arbitration lives on @c PowerSupervisor;
 * this class is reduced to wiring those subsystems together,
 * registering their fds with the loop, and mediating teardown order.
 *
 * Adapter ownership: the daemon constructs the @c LibCecAdapter on
 * the main thread, runs @c initialize() and @c openConnection() on
 * the main thread (libcec's @c Open is thread-identity-agnostic),
 * hands the opened adapter to @c AdapterWorker, then builds the
 * lifecycle, dispatcher, and supervisor. From @c m_worker->start()
 * onwards the worker is the sole thread that invokes any other libcec
 * method; callbacks arriving between @c Open() returning and the
 * supervisor being assigned are absorbed by null checks in the
 * daemon's forwarders.
 *
 * Shutdown drives a strict ordering so no thread observes a
 * destroyed subsystem: the socket server stops before the dispatcher
 * and lifecycle shutdown gates are flipped, the worker stops (closing
 * the adapter on its exit path) before subsystems are released, and
 * unique_ptrs are reset with @c m_supervisor first so that its
 * non-owning references to the dispatcher / lifecycle / worker / dbus
 * monitor are dropped before any of those is itself destroyed.
 */
class CECDaemon {
public:
    /**
     * Take ownership of a parsed @c AppConfig snapshot. The daemon
     * retains it for the process lifetime so that a future SIGHUP
     * reload can diff against the initial values.
     */
    explicit CECDaemon(AppConfig config);
    ~CECDaemon();

    CECDaemon(const CECDaemon&)            = delete;
    CECDaemon& operator=(const CECDaemon&) = delete;

    /**
     * Construct, initialise, and open the adapter on the main thread;
     * hand the opened adapter to the worker; build the lifecycle,
     * dispatcher, and supervisor; start the worker; register every
     * event-loop source. Returns @c true on success; a failure path
     * leaves the daemon in a state where @c stop() will tear down
     * whatever partial state was committed.
     */
    [[nodiscard]] bool start();

    /** Block on the event loop until a signal or error exits it. */
    void run();

    /** Tear everything down. Idempotent; safe after a failed @c start(). */
    void stop();

    /**
     * Process exit status to return from @c main. @c EXIT_FAILURE iff
     * the daemon terminated itself due to an unrecoverable subsystem
     * condition (currently: @c PowerSupervisor abandoning the CEC
     * reconnect cycle after its backoff schedule is exhausted);
     * @c EXIT_SUCCESS for a clean signal-driven stop. Meaningful only
     * after @c run() has returned. Latched one-way: a SIGTERM arriving
     * after the failure latch does not promote the status back to
     * success.
     */
    [[nodiscard]] int exitStatus() const noexcept { return m_exitStatus; }

private:
    /** Handler for signalfd readability. */
    void onSignalReadable();

    /**
     * Handler for the systemd watchdog timer. Drains the timerfd and
     * sends @c WATCHDOG=1 to the service manager. Only registered with
     * the event loop when the unit actually configured @c WatchdogSec;
     * otherwise the timer stays disarmed and this method is never called.
     */
    void onWatchdogTimerFired();

    /**
     * Route an incoming wire command. Runs on the main thread.
     * Suspend and resume delegate directly to @c PowerSupervisor and
     * reply inline; everything else is forwarded to the dispatcher
     * whose @c dispatch chooses between an inline reply and a worker
     * hop.
     */
    void handleCommand(Message command, ResponseSink reply);

    /**
     * Adapter callback forwarder: CEC bus observation. Fires on
     * libcec's command thread; hops the observation through @c m_work
     * so every subscriber (@c StandbyPolicy today, plus future
     * consumers such as the hook subsystem) runs on the main thread
     * with single-threaded semantics. The daemon owns the forwarder
     * rather than wiring libcec directly to each subscriber so the
     * callback target is stable across subsystem construction order
     * and adding a subscriber stays a local change.
     */
    void onAdapterObservation(ICecAdapter::Observation obs);

    /**
     * Adapter callback forwarder: connection lost. Fires on libcec's
     * alert thread; hops the observation through @c m_work so the
     * supervisor's reconnect FSM transition runs on the main thread.
     */
    void onAdapterConnectionLost();

    /** Ensure a DBus monitor is up; returns @c true on success. */
    [[nodiscard]] bool setupPowerMonitor();

    /**
     * Signal a daemon-level shutdown driven by an unrecoverable
     * subsystem condition. Latches @c m_exitStatus to @c EXIT_FAILURE
     * (first-fire wins; subsequent invocations are absorbed so a
     * second abandonment cycle cannot rewrite the reason) and asks
     * the event loop to stop. The main-thread teardown then unwinds
     * via @c run() returning and @c stop() running its ordered
     * sequence. Main thread only — the wired supervisor callback runs
     * inside the supervisor's own main-thread event handler.
     */
    void requestUnrecoverableShutdown();

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
    TimerSource    m_reconnectRetryTimer;
    // Fires the systemd watchdog ping at half the configured WatchdogSec.
    // Registered with the loop only when a watchdog is actually configured
    // (see CECDaemon::start); otherwise the timerfd stays inert.
    TimerSource    m_watchdogTimer;

    // Auto-suspend-on-TV-standby policy. Declared before m_worker so
    // reverse-of-declaration destruction joins libcec's command
    // thread first: no observation closure the libcec thread queued
    // on @c m_work can still be drained by the time the policy is
    // destroyed, because the event loop has already stopped. The
    // null check inside the forwarder's posted closure is belt-and-
    // braces for the in-construction window between libcec's Open()
    // and this assignment.
    std::unique_ptr<StandbyPolicy> m_standbyPolicy;

    // Hook executor and the CEC hook subsystem that feeds it.
    //
    // Destruction ordering — @c stop() below mirrors this explicitly
    // and the field order here is its fallback:
    //
    //   * @c m_hooks holds a reference to @c m_hookExecutor (it
    //     submits jobs through it), so @c m_hooks must be destroyed
    //     first. Hence @c m_hookExecutor declared AFTER @c m_hooks is
    //     wrong; keep them in this order (executor first, subsystem
    //     second) so reverse-of-declaration gives us subsystem →
    //     executor.
    //   * @c m_hookExecutor owns a worker thread that only consumes
    //     its own queue and never calls back into the daemon — stop()
    //     joins it independently of libcec and signalfd.
    //
    // Also declared before @c m_worker: observation closures that the
    // worker / libcec path posts through @c m_work land on the main
    // thread and can touch @c m_hooks, so the hooks must outlive any
    // such in-flight closure. The @c stop() sequence guarantees this
    // by resetting @c m_worker (which drains libcec) before @c m_hooks
    // / @c m_hookExecutor.
    std::unique_ptr<HookExecutor>     m_hookExecutor;
    std::unique_ptr<CecHookSubsystem> m_hooks;

    // CEC adapter actor. Owns the libcec handle and the single thread
    // that is allowed to touch it. Built before the lifecycle and
    // dispatcher so they can capture references; the adapter itself
    // is handed into the worker at construction. Destroyed in
    // @c stop(), which joins the worker thread and closes the adapter
    // on its exit path.
    std::unique_ptr<AdapterWorker> m_worker;

    // Domain subsystems. Declaration order matters for reverse-of-
    // declaration destruction: the dispatcher holds a reference to
    // the lifecycle, and both hold references to the worker, so the
    // dispatcher must be torn down before the lifecycle and both
    // before the worker. @c stop() resets them in the same order
    // explicitly.
    std::unique_ptr<AdapterLifecycle>  m_lifecycle;
    std::unique_ptr<CommandDispatcher> m_dispatcher;
    std::unique_ptr<SocketServer>      m_socketServer;
    std::unique_ptr<DBusMonitor>       m_dbusMonitor;

    // Power lifecycle / reconnect orchestrator. Holds non-owning refs
    // to the dispatcher, the adapter lifecycle, the worker, the work
    // queue, and three timers, plus a may-be-null pointer to the dbus
    // monitor (wired in setupPowerMonitor and cleared on attach
    // failure / shutdown). Declared after the subsystems above so
    // reverse-of-declaration destruction would tear it down first;
    // @c stop() resets it explicitly first as well.
    std::unique_ptr<PowerSupervisor> m_supervisor;

    // Parsed configuration snapshot. Stored by value so consumers can
    // receive copies (AdapterConfig) or a const-ref (CommandDispatcher)
    // without any of them invalidating the daemon's view. A future
    // SIGHUP reload diffs a freshly-loaded AppConfig against this.
    AppConfig m_config;

    // True between start() returning success and stop() completing.
    bool m_started = false;

    // Process exit status surfaced via exitStatus(). Latched one-way
    // from EXIT_SUCCESS to EXIT_FAILURE by requestUnrecoverableShutdown
    // the first time an unrecoverable subsystem condition is raised;
    // subsequent latch attempts are no-ops.
    int m_exitStatus = EXIT_SUCCESS;
};

} // namespace cec_control
