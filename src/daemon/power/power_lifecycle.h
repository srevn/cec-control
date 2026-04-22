#pragma once

#include <chrono>
#include <deque>

namespace cec_control {

/**
 * @class PowerLifecycle
 * @brief Pure decision type for the suspend / resume sequencer.
 *
 * One lifecycle event is in flight at a time; subsequent events queue
 * until the active phase returns to Idle. Events flow in via per-method
 * handlers and produce a typed @c Output describing the side effects
 * the daemon must carry out (start work on the pool, arm / disarm
 * timers, release / take the logind inhibit lock, notify the reconnect
 * FSM, submit a late reconnect attempt).
 *
 * Thread-safety: main-thread only. Callers serialise implicitly via
 * the event loop; no mutex is held.
 *
 * ## Orthogonal effect axes
 *
 * A single transition legitimately affects multiple subsystems: exiting
 * Suspending must disarm the safety timer and release the inhibit lock
 * in the same step. The @c Output struct reflects this with one axis
 * per subsystem, each with its own small enum; contradictory
 * combinations on a single axis are unrepresentable ("Arm and Disarm"
 * on the safety timer in a single output cannot be expressed).
 *
 * ## Coalescing rule
 *
 * An incoming @c Suspend / @c Resume is dropped iff its @c (kind,
 * source) matches the effective tail of the pipeline — the back of the
 * pending queue if non-empty, or the in-flight phase otherwise. The
 * match MUST include the @c Source: a @c Suspend(Wire) in flight does
 * not subsume a subsequent @c Suspend(DBus), because only the DBus
 * cycle carries the obligation to release the logind inhibit lock.
 *
 * ## Safety-timer ownership
 *
 * For a @c Suspending(DBus) cycle the inhibit-lock release obligation
 * can be discharged by either the completion path or the safety timer,
 * whichever fires first. @c m_safetyFiredFirst records which side won;
 * completion branches on it to pick between the happy log and the
 * overrun log. @c onSafetyTimerArmFailed is a deliberate no-op — the
 * timer simply will not fire, so the completion path naturally takes
 * the happy branch and performs the release.
 */
class PowerLifecycle {
public:
    /**
     * Origin of a suspend / resume request. DBus events are bound to
     * a real logind sleep cycle and must release / retake the delay
     * inhibitor. Wire events are local operator actions and leave the
     * lock alone.
     */
    enum class Source { DBus, Wire };

    /**
     * Side effects the dispatcher must carry out on the daemon's
     * subsystems. Default-constructed instances are inert.
     */
    struct Output {
        enum class Work   { None, StartSuspend, StartResume };
        enum class Lock   { None, Release, Take };
        enum class Timer  { None, Arm, Disarm };
        enum class Notify { None, SystemSuspend, SystemResume };

        /**
         * Safety-timer outcome on this transition. The "fired XOR
         * overrun" invariant becomes unrepresentable by construction.
         *
         *  - @c Inert    no safety-timer observation this transition.
         *  - @c Fired    @c onSafetyTimerFired observed a live
         *                (non-stale) firing. Dispatcher gates the
         *                "releasing inhibit lock forcibly" warning
         *                on this.
         *  - @c Overrun  @c onSuspendCompleted observed that the
         *                safety timer had already fired this cycle.
         *                Dispatcher picks the overrun log on this.
         */
        enum class SafetyOutcome { Inert, Fired, Overrun };

        Work   work = Work::None;
        Lock   lock = Lock::None;

        Timer  safetyTimer = Timer::None;
        std::chrono::milliseconds safetyDelay{};
        Timer  resumeRetry = Timer::None;
        std::chrono::milliseconds resumeRetryDelay{};

        bool   submitLateReconnect = false;
        Notify reconnectNotify     = Notify::None;

        SafetyOutcome safety = SafetyOutcome::Inert;
    };

    /** Deadline after which the safety timer releases the inhibit lock. */
    static constexpr auto kSuspendSafetyDeadline = std::chrono::seconds(10);

    /** Delay before the single post-resume reconnect attempt. */
    static constexpr auto kResumeRetryDelay = std::chrono::seconds(10);

    PowerLifecycle() noexcept = default;

    PowerLifecycle(const PowerLifecycle&) = delete;
    PowerLifecycle& operator=(const PowerLifecycle&) = delete;

    /** A client has requested a suspend. */
    [[nodiscard]] Output onSuspendRequested(Source source) noexcept;

    /** A client has requested a resume. */
    [[nodiscard]] Output onResumeRequested(Source source) noexcept;

    /** The suspend pool task has completed (duration logged by caller). */
    [[nodiscard]] Output onSuspendCompleted() noexcept;

    /** The resume pool task has completed; @p adapterValid is isConnected(). */
    [[nodiscard]] Output onResumeCompleted(bool adapterValid) noexcept;

    /** The suspend-safety timerfd became readable. */
    [[nodiscard]] Output onSafetyTimerFired() noexcept;

    /** The resume-retry timerfd became readable. */
    [[nodiscard]] Output onResumeRetryTimerFired() noexcept;

    /** Dispatcher feedback: arming the safety timerfd failed. */
    [[nodiscard]] Output onSafetyTimerArmFailed() noexcept;

    /** Dispatcher feedback: arming the resume-retry timerfd failed. */
    [[nodiscard]] Output onResumeRetryArmFailed() noexcept;

    /**
     * If Idle with a non-empty queue, pop and emit the entry Output
     * for the next event. Otherwise returns an inert Output. The
     * dispatcher calls this once after applying every Output; a
     * single call suffices because a successful pump leaves the FSM
     * non-Idle, blocking further pumps.
     */
    [[nodiscard]] Output pumpQueue() noexcept;

private:
    enum class Phase     { Idle, Suspending, Resuming };
    enum class EventKind { Suspend, Resume };

    struct Pending {
        EventKind kind;
        Source    source;
    };

    [[nodiscard]] bool coalescesWithEffectiveTail(
        EventKind kind, Source source) const noexcept;

    [[nodiscard]] Output makeSuspendEntry(Source source) noexcept;
    [[nodiscard]] Output makeResumeEntry(Source source) noexcept;

    Phase  m_phase = Phase::Idle;
    Source m_phaseSource = Source::DBus;
    bool   m_safetyFiredFirst = false;
    bool   m_resumeRetryArmed = false;
    std::deque<Pending> m_pending;
};

} // namespace cec_control
