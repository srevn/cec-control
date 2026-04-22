#pragma once

#include <chrono>
#include <functional>

#include "../../common/backoff_schedule.h"
#include "adapter_reconnect.h"
#include "power_lifecycle.h"

namespace cec_control {

class AdapterLifecycle;
class AdapterWorker;
class CommandDispatcher;
class DBusMonitor;
class TimerSource;

/**
 * @class PowerSupervisor
 * @brief Owns the suspend/resume lifecycle FSM and the adapter
 *        reconnect FSM, and translates their pure decisions into
 *        side effects against the daemon's subsystems.
 *
 * Both FSMs are pure decision types: they emit @c Output values
 * describing the side effects to execute. This class is the executor.
 * It holds non-owning references to the timers, the adapter worker,
 * the adapter lifecycle, and the command dispatcher, plus a may-be-
 * null pointer to the D-Bus monitor (which is constructed conditionally
 * on the @c [Daemon] config flag and may also be torn down on attach
 * failure). The supervisor is itself owned by @c CECDaemon and never
 * outlives any of those subsystems.
 *
 * ## Threading
 *
 * Main-thread only. Every public entry point runs on the thread that
 * drives the event loop. Worker-completion callbacks installed by
 * @c submitSuspendWork / @c submitResumeWork / @c submitReconnectAttempt
 * are routed back to the main thread by @c AdapterLifecycle (which
 * posts via @c MainThreadWork inside each worker job), so the lambdas
 * this class hands the lifecycle execute on main as well. The
 * supervisor introduces no new threading; it is pure choreography
 * over the daemon's pre-existing single-threaded effect surface.
 *
 * ## Effect ordering
 *
 * @c executeEffects walks the @c PowerLifecycle::Output axes in the
 * deliberate order chosen for reviewability:
 *  1. log the entry banner for the new cycle (so concurrent timer-arm
 *     warnings land after "starting X" in the log);
 *  2. disarm stale timers before re-arming them on the same axis;
 *  3. notify the reconnect FSM (a @c SystemSuspend / @c SystemResume
 *     there cancels any armed reconnect retry, including a seeded
 *     post-resume retry waiting on its delay);
 *  4. arm new timers, feeding @c onSafetyTimerArmFailed back into
 *     the lifecycle FSM on syscall failure;
 *  5. apply inhibit-lock operations (no-op when @c m_dbusMonitor is
 *     null);
 *  6. submit adapter-side work (suspend / resume).
 *
 * Preserve this ordering verbatim: subtle invariants ride on it (e.g.
 * the safety timer must be disarmed before the lock release that
 * follows the FSM's overrun branch, or a stale fire could re-trigger
 * the warning path after the lock has already been dropped).
 *
 * ## Post-resume reconnect
 *
 * When @c onResumeCompleted observes @c adapterValid == false, the
 * supervisor seeds @c AdapterReconnect with @c kPostResumeRetryDelay
 * as the initial-attempt delay. From that point the unified reconnect
 * cycle owns the backoff (attempt 1 after the seed delay, then
 * schedule-driven retries). Prior to unification this path ran a
 * separate single-shot retry timer alongside the alert-driven FSM;
 * both could fire concurrently. The unified model eliminates the race.
 *
 * ## Unrecoverable escalation
 *
 * When @c AdapterReconnect abandons its cycle after exhausting the
 * backoff schedule, the supervisor invokes the install-once
 * @c AdapterUnrecoverableCallback supplied at construction. The
 * callback's owner (the daemon) decides what "unrecoverable" means
 * in policy terms — today: request a clean process shutdown so a
 * service manager such as systemd can restart us into a fresh libcec
 * state. The supervisor itself only signals; no policy lives here.
 */
class PowerSupervisor {
public:
    /**
     * Fires on the main thread when @c AdapterReconnect abandons its
     * cycle after exhausting the backoff schedule. Invoked inside the
     * supervisor's own event handler, so the callback may call any
     * main-thread API including @c EventLoop::stop. Install-once: set
     * at construction and never reassigned. May be empty, in which
     * case the abandon path only logs — useful for future isolated
     * tests of the supervisor.
     */
    using AdapterUnrecoverableCallback = std::function<void()>;

    /**
     * Capture references to every subsystem the supervisor must
     * coordinate. The references must outlive @c this; the daemon
     * achieves this by destroying the supervisor first in @c stop()
     * and by declaring it after the timers / dispatcher / lifecycle /
     * worker.
     *
     * The D-Bus monitor pointer is intentionally absent here — it is
     * wired post-construction via @c setDBusMonitor once
     * @c setupPowerMonitor decides whether power monitoring is even
     * enabled this run.
     *
     * @p onAdapterUnrecoverable is stored by value (move-in) and is
     * immutable thereafter. See @c AdapterUnrecoverableCallback for
     * the contract; empty is acceptable and makes abandonment a
     * log-only event.
     */
    PowerSupervisor(CommandDispatcher& dispatcher,
                    AdapterLifecycle&  lifecycle,
                    AdapterWorker&     worker,
                    TimerSource&       suspendSafety,
                    TimerSource&       reconnectRetry,
                    AdapterUnrecoverableCallback onAdapterUnrecoverable) noexcept;

    ~PowerSupervisor() = default;

    PowerSupervisor(const PowerSupervisor&)            = delete;
    PowerSupervisor& operator=(const PowerSupervisor&) = delete;

    /**
     * Inject (or clear) the D-Bus monitor pointer. Call with a valid
     * pointer right after the monitor has been initialised; call with
     * @c nullptr before the monitor is destroyed (or when an attach
     * failure tears it back down inside @c CECDaemon::start). All
     * inhibit-lock operations are silent no-ops while the pointer is
     * null.
     */
    void setDBusMonitor(DBusMonitor* dbusMonitor) noexcept;

    /**
     * Wire / DBus-source: a suspend has been requested. Feeds the
     * lifecycle FSM and applies the resulting output. Main thread only.
     */
    void onSuspendRequested(PowerLifecycle::Source source);

    /** Wire / DBus-source: a resume has been requested. Main thread only. */
    void onResumeRequested(PowerLifecycle::Source source);

    /**
     * Worker-completion handler for the suspend phase. Public so the
     * lambda installed in @c submitSuspendWork (which posts completion
     * to the main thread via @c MainThreadWork::post) can name it.
     * Reads the lifecycle FSM's outcome to choose between the happy
     * log and the overrun log, then applies the resulting output.
     */
    void onSuspendCompleted(std::chrono::milliseconds workDuration);

    /** Worker-completion handler for the resume phase. */
    void onResumeCompleted(bool adapterValid);

    /** The suspend-safety timerfd became readable. */
    void onSafetyTimerFired();

    /** The reconnect-retry timerfd became readable. */
    void onReconnectRetryTimerFired();

    /**
     * Main-thread entry point for a libcec connection-lost alert. The
     * libcec callback fires on the alert thread; the daemon hops it
     * through @c MainThreadWork::post and lands here.
     */
    void onConnectionLost();

    /** Worker-completion handler for a single reconnect attempt. */
    void onReconnectResult(bool ok);

    /**
     * @c true iff the adapter is currently considered suspended. Reads
     * @c AdapterLifecycle's @c SuspendQueue flag — the actual gate
     * that routes inbound dispatches into the queue; the lifecycle
     * FSM's @c Phase is a finer-grained "in-progress" view that does
     * not match this boundary post-completion.
     */
    [[nodiscard]] bool isSuspended() const noexcept;

private:
    /**
     * Apply an @c Output emitted by the lifecycle FSM, then pump any
     * queued event via @c PowerLifecycle::pumpQueue. A single
     * post-event pump suffices because a successful pump leaves the
     * FSM non-Idle, blocking further pumps.
     */
    void applyLifecycle(PowerLifecycle::Output output);

    /**
     * Flat dispatcher over the lifecycle output's axes. The execution
     * order is documented at the class level — preserve it.
     */
    void executeEffects(const PowerLifecycle::Output& output);

    /** Kick off @c AdapterLifecycle::suspendAsync with completion wiring. */
    void submitSuspendWork();

    /**
     * Kick off @c AdapterLifecycle::resumeAsync. The completion lambda
     * receives the drained queue alongside the adapter validity and
     * hands it to @c CommandDispatcher::replay before forwarding the
     * adapter validity to @c onResumeCompleted for the FSM transition.
     */
    void submitResumeWork();

    /** Submit one reconnect attempt; the result lands in @c onReconnectResult. */
    void submitReconnectAttempt();

    /** Carry out the side effect emitted by the reconnect FSM. */
    void execute(AdapterReconnect::Output out);

    /**
     * Delay before the seeded post-resume reconnect attempt. Covers
     * USB re-enumeration settling after wake; subsequent retries
     * follow @c AdapterReconnect's backoff schedule.
     */
    static constexpr auto kPostResumeRetryDelay = std::chrono::seconds(10);

    CommandDispatcher& m_dispatcher;
    AdapterLifecycle&  m_lifecycle;
    AdapterWorker&     m_worker;
    TimerSource&       m_suspendSafetyTimer;
    TimerSource&       m_reconnectRetryTimer;

    // Wired post-construction; null until setupPowerMonitor succeeds,
    // and reset back to null on attach failure or shutdown teardown.
    DBusMonitor* m_dbusMonitor = nullptr;

    // Install-once notification hook for reconnect abandonment. Moved
    // in at construction, never reassigned. @c const documents the
    // install-once contract; the invocation site null-checks so an
    // empty callback is legal.
    const AdapterUnrecoverableCallback m_onAdapterUnrecoverable;

    // Pure decision types driving suspend/resume arbitration and the
    // connection-lost reconnect cycle. Both main-thread only; no
    // atomics or mutexes. The supervisor is the sole executor of their
    // outputs. Named to match their class types so they are unambiguous
    // alongside @c m_lifecycle (the @c AdapterLifecycle reference).
    PowerLifecycle   m_powerLifecycle;
    AdapterReconnect m_adapterReconnect{BackoffSchedule{
        std::chrono::seconds(5),
        std::chrono::seconds(10),
        std::chrono::seconds(20),
    }};
};

} // namespace cec_control
