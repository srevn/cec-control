#include "adapter_reconnect.h"

#include <utility>

namespace cec_control {

AdapterReconnect::AdapterReconnect(BackoffSchedule schedule) noexcept
    : m_schedule(std::move(schedule)) {}

void AdapterReconnect::resetCycle() noexcept {
    m_state = State::Idle;
    m_schedule.reset();
    m_nextAttemptNumber = 0;
}

AdapterReconnect::Output
AdapterReconnect::seedCycle(std::chrono::milliseconds initialDelay) noexcept {
    // Non-Idle: a cycle is already in flight; absorb. This is the
    // correct behaviour for the concurrent case where a libcec alert
    // beats the post-resume completion through MainThreadWork — the
    // running cycle already covers the adapter-gone condition.
    if (m_state != State::Idle) return {};

    // Defensive: a clean entry into Idle resets the schedule (see
    // resetCycle), but explicit reset documents that the seeded cycle
    // starts at schedule index 0 and mirrors the sibling reset on
    // ConnectionLost from WaitingForRetry.
    m_schedule.reset();
    m_state = State::WaitingForRetry;
    m_nextAttemptNumber = 1;
    return {Effect::ScheduleRetry, initialDelay,
            /*attemptNumber*/ 1, totalAttempts()};
}

AdapterReconnect::Output AdapterReconnect::onEvent(Event event) noexcept {
    const std::size_t total = totalAttempts();

    switch (m_state) {
    case State::Idle:
        // A fresh ConnectionLost starts a new cycle; every other event
        // is stale (an attempt result or timer tick that raced a
        // transition into Idle) or inapplicable (suspend/resume while
        // already at rest).
        if (event == Event::ConnectionLost) {
            m_state = State::Attempting;
            m_nextAttemptNumber = 1;
            return {Effect::StartAttempt, {}, /*attemptNumber*/ 1, total};
        }
        return {};

    case State::Attempting:
        switch (event) {
        case Event::AttemptSucceeded:
            resetCycle();
            return {};

        case Event::AttemptFailed: {
            const auto attempt = m_schedule.nextDelay();
            if (attempt) {
                // Overall attempt number is one higher than the
                // schedule-local index: the immediate attempt after
                // ConnectionLost is overall attempt 1 and does not
                // draw from the schedule.
                m_state = State::WaitingForRetry;
                m_nextAttemptNumber = attempt->index + 1;
                return {Effect::ScheduleRetry, attempt->delay,
                        m_nextAttemptNumber, total};
            }
            resetCycle();
            return {Effect::AbandonCycle, {}, 0, total};
        }

        case Event::SystemSuspend:
        case Event::SystemResume:
            // No timer armed in this state; the in-flight pool task
            // will land and be absorbed as a stale result by Idle.
            resetCycle();
            return {};

        case Event::ConnectionLost:
            // The in-flight attempt already addresses the same
            // condition; do not double-submit.
            return {};

        case Event::RetryTimerFired:
        case Event::TimerArmFailed:
            // Unreachable under normal flow. Absorb defensively: a
            // timerfd read may race a disarm via epoll readiness.
            return {};
        }
        break;

    case State::WaitingForRetry:
        switch (event) {
        case Event::ConnectionLost:
            // A fresh disconnect supersedes the pending retry; start
            // the cycle over. StartAttempt implies the armed timer is
            // disarmed by the dispatcher.
            m_schedule.reset();
            m_state = State::Attempting;
            m_nextAttemptNumber = 1;
            return {Effect::StartAttempt, {}, /*attemptNumber*/ 1, total};

        case Event::RetryTimerFired:
            // m_nextAttemptNumber was captured when this timer was
            // armed in the preceding AttemptFailed transition.
            m_state = State::Attempting;
            return {Effect::StartAttempt, {},
                    m_nextAttemptNumber, total};

        case Event::SystemSuspend:
        case Event::SystemResume:
            resetCycle();
            return {Effect::CancelRetry, {}, 0, 0};

        case Event::TimerArmFailed:
            // Dispatcher could not arm the timer; the cycle is over.
            resetCycle();
            return {Effect::AbandonCycle, {}, 0, total};

        case Event::AttemptSucceeded:
        case Event::AttemptFailed:
            // Attempt results only originate from Attempting. Absorb
            // defensively.
            return {};
        }
        break;
    }

    return {};
}

} // namespace cec_control
