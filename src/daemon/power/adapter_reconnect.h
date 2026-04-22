#pragma once

#include <chrono>
#include <cstddef>

#include "../../common/backoff_schedule.h"

namespace cec_control {

/**
 * @class AdapterReconnect
 * @brief Pure decision type for the adapter reconnect cycle.
 *
 * Events flow in via @c onEvent and are translated into typed effects
 * that the daemon carries out (submit an attempt, arm a retry timer,
 * disarm one, log an abandonment). The type owns its backoff schedule
 * and its @c State; it has no knowledge of timers, threads, the thread
 * pool, or libCEC.
 *
 * Thread-safety: main-thread only. Callers serialise implicitly via
 * the event loop; no mutex is held.
 *
 * Semantics to note:
 *  - The first attempt of a cycle is triggered by @c ConnectionLost
 *    (delayed by the @c connectionLostDelay captured at construction,
 *    or immediate when that delay is zero) or @c seedCycle (delayed by
 *    the caller-supplied @p initialDelay). Neither draws from the
 *    schedule; the schedule covers attempts 2..N. Total attempts is
 *    therefore @c schedule.size() + 1 for both entry points.
 *  - @c StartAttempt implies that any armed retry timer is stale. The
 *    dispatcher must @c disarm() the timer before submitting the
 *    attempt; @c TimerSource::disarm is idempotent so the effect
 *    handler can call it unconditionally.
 *  - Stale @c AttemptSucceeded / @c AttemptFailed results arriving
 *    after a lifecycle transition are absorbed as no-ops: the FSM has
 *    already moved to @c Idle in response to @c SystemSuspend /
 *    @c SystemResume, and @c Idle answers every non-@c ConnectionLost
 *    event with @c Effect::None.
 *
 * ## Scope vs. CMD_RESTART_ADAPTER
 *
 * CMD_RESTART_ADAPTER bypasses this FSM entirely. Operator-triggered
 * restart is demand with no backoff and no retry policy; it submits a
 * single reopen on the worker FIFO and replies inline. A restart
 * landing while this FSM is mid-cycle simply queues behind any
 * in-flight reopen on the worker; the FSM's pending attempt resolves
 * with whatever state the adapter ends up in. Not unified with this
 * FSM because "recovery" and "demand" have different failure policies.
 */
class AdapterReconnect {
public:
    /**
     * Input events. Driven by fd handlers (connection loss, retry
     * timer), pool-task continuations (attempt succeeded/failed), and
     * lifecycle transitions (suspend/resume).
     */
    enum class Event {
        ConnectionLost,
        AttemptSucceeded,
        AttemptFailed,
        SystemSuspend,
        SystemResume,
        RetryTimerFired,
        TimerArmFailed,   ///< Dispatcher failed to arm the retry timer.
    };

    /**
     * Side-effect the dispatcher must carry out.
     *
     *  - @c None            do nothing.
     *  - @c StartAttempt    submit a reconnect attempt (implies: disarm
     *                       the retry timer first).
     *  - @c ScheduleRetry   arm the retry timer with @c Output::delay.
     *  - @c CancelRetry     disarm the retry timer.
     *  - @c AbandonCycle    log that the cycle has been abandoned.
     */
    enum class Effect {
        None,
        StartAttempt,
        ScheduleRetry,
        CancelRetry,
        AbandonCycle,
    };

    /**
     * Result of a transition.
     *
     *  - @c delay           set only for @c ScheduleRetry.
     *  - @c attemptNumber   the attempt the dispatcher is about to act
     *                       on (start or schedule); unset for
     *                       @c CancelRetry / @c None.
     *  - @c totalAttempts   attempts in the active cycle (size()+1);
     *                       always set when any of the above are.
     */
    struct Output {
        Effect effect = Effect::None;
        std::chrono::milliseconds delay{};
        std::size_t attemptNumber = 0;
        std::size_t totalAttempts = 0;
    };

    /**
     * @param schedule             backoff schedule covering attempts 2..N.
     * @param connectionLostDelay  delay to apply before the very first
     *                             attempt of a cycle triggered by
     *                             @c Event::ConnectionLost. Zero (the
     *                             default) preserves the immediate-attempt
     *                             semantics; non-zero lets hardware/udev
     *                             settle before libcec is poked — the
     *                             difference between cheaply discovering
     *                             the adapter is gone and paying libcec's
     *                             multi-second @c Open() ceremony.
     */
    explicit AdapterReconnect(
        BackoffSchedule schedule, std::chrono::milliseconds connectionLostDelay =
                                  std::chrono::milliseconds::zero()) noexcept;

    /** Feed an event; receive the resulting effect. */
    [[nodiscard]] Output onEvent(Event event) noexcept;

    /**
     * Seed a reconnect cycle whose first attempt is delayed by
     * @p initialDelay. Covers the post-resume path where the resume
     * worker's reopen returned a disconnected adapter and a delayed
     * retry is needed to let USB re-enumeration settle.
     *
     * The seeded attempt bypasses @c schedule.nextDelay(); on failure
     * the full schedule runs from attempt 2 onward, yielding
     * @c totalAttempts() attempts total — identical shape to the
     * @c ConnectionLost-driven cycle, so the only difference is
     * whether attempt 1 fires immediately or after a delay.
     *
     * Distinct from @c onEvent(ConnectionLost): @c seedCycle absorbs
     * silently (returns @c Effect::None) if the FSM is non-Idle — a
     * cycle is already covering the same adapter-gone condition —
     * whereas @c ConnectionLost from @c WaitingForRetry supersedes
     * the pending retry with an immediate attempt.
     */
    [[nodiscard]] Output seedCycle(std::chrono::milliseconds initialDelay) noexcept;

private:
    /**
     * Internal lifecycle. @c Idle is the rest state; @c Attempting is
     * "a reopen is in flight on the adapter worker"; @c
     * WaitingForRetry is "the retry timer is armed for the next
     * attempt".
     */
    enum class State { Idle, Attempting, WaitingForRetry };

    /** Total attempts in the active cycle. Immediate first + scheduled N. */
    [[nodiscard]] std::size_t totalAttempts() const noexcept {
        return m_schedule.size() + 1;
    }

    /** Return to @c Idle with a fresh schedule and no pending attempt. */
    void resetCycle() noexcept;

    State           m_state = State::Idle;
    BackoffSchedule m_schedule;
    /**
     * Delay applied to the first attempt when @c ConnectionLost fires
     * from @c Idle. Immutable after construction. Zero disables the
     * delay — the FSM then fires @c StartAttempt immediately, matching
     * the pre-delay behaviour.
     */
    const std::chrono::milliseconds m_connectionLostDelay;
    /**
     * 1-based overall attempt number captured when the retry timer
     * is armed, consumed when the timer fires. Zero outside of an
     * active cycle. Kept on the FSM because the arming
     * (@c AttemptFailed) and firing (@c RetryTimerFired) happen in
     * different @c onEvent invocations; the schedule deliberately
     * exposes no post-advance peek API.
     */
    std::size_t m_nextAttemptNumber = 0;
};

} // namespace cec_control
