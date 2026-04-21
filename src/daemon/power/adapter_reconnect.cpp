#include "adapter_reconnect.h"

#include <utility>

namespace cec_control {

AdapterReconnect::AdapterReconnect(BackoffSchedule schedule) noexcept
    : m_schedule(std::move(schedule)) {}

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
            return {Effect::StartAttempt, {}, /*attemptNumber*/ 1, total};
        }
        return {};

    case State::Attempting:
        switch (event) {
        case Event::AttemptSucceeded:
            m_state = State::Idle;
            m_schedule.reset();
            return {};

        case Event::AttemptFailed: {
            const auto delay = m_schedule.nextDelay();
            if (delay) {
                // attemptsSoFar() was incremented by nextDelay(); +1
                // names the attempt number the retry timer will fire.
                m_state = State::WaitingForRetry;
                return {Effect::ScheduleRetry, *delay,
                        m_schedule.attemptsSoFar() + 1, total};
            }
            m_state = State::Idle;
            m_schedule.reset();
            return {Effect::AbandonCycle, {}, 0, total};
        }

        case Event::SystemSuspend:
        case Event::SystemResume:
            // No timer armed in this state; the in-flight pool task
            // will land and be absorbed as a stale result by Idle.
            m_state = State::Idle;
            m_schedule.reset();
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
            return {Effect::StartAttempt, {}, /*attemptNumber*/ 1, total};

        case Event::RetryTimerFired:
            // attemptsSoFar() is unchanged since we armed this timer;
            // +1 names the attempt we are about to run.
            m_state = State::Attempting;
            return {Effect::StartAttempt, {},
                    m_schedule.attemptsSoFar() + 1, total};

        case Event::SystemSuspend:
        case Event::SystemResume:
            m_state = State::Idle;
            m_schedule.reset();
            return {Effect::CancelRetry, {}, 0, 0};

        case Event::TimerArmFailed:
            // Dispatcher could not arm the timer; the cycle is over.
            m_state = State::Idle;
            m_schedule.reset();
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
