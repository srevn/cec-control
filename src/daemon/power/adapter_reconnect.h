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
 *    and does not draw from the schedule. Total attempts is therefore
 *    @c schedule.size() + 1.
 *  - @c StartAttempt implies that any armed retry timer is stale. The
 *    dispatcher must @c disarm() the timer before submitting the
 *    attempt; @c TimerSource::disarm is idempotent so the effect
 *    handler can call it unconditionally.
 *  - Stale @c AttemptSucceeded / @c AttemptFailed results arriving
 *    after a lifecycle transition are absorbed as no-ops: the FSM has
 *    already moved to @c Idle in response to @c SystemSuspend /
 *    @c SystemResume, and @c Idle answers every non-@c ConnectionLost
 *    event with @c Effect::None.
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

    explicit AdapterReconnect(BackoffSchedule schedule) noexcept;

    /** Feed an event; receive the resulting effect. */
    [[nodiscard]] Output onEvent(Event event) noexcept;

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
