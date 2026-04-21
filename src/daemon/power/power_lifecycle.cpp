#include "power_lifecycle.h"

namespace cec_control {

bool PowerLifecycle::coalescesWithEffectiveTail(
        EventKind kind, Source source) const noexcept {
    if (!m_pending.empty()) {
        const auto& tail = m_pending.back();
        return tail.kind == kind && tail.source == source;
    }
    if (m_phase == Phase::Idle) return false;

    const EventKind activeKind = (m_phase == Phase::Suspending)
                                 ? EventKind::Suspend
                                 : EventKind::Resume;
    return activeKind == kind && m_phaseSource == source;
}

PowerLifecycle::Output
PowerLifecycle::onSuspendRequested(Source source) noexcept {
    if (!coalescesWithEffectiveTail(EventKind::Suspend, source)) {
        m_pending.push_back({EventKind::Suspend, source});
    }
    return {};
}

PowerLifecycle::Output
PowerLifecycle::onResumeRequested(Source source) noexcept {
    if (!coalescesWithEffectiveTail(EventKind::Resume, source)) {
        m_pending.push_back({EventKind::Resume, source});
    }
    return {};
}

PowerLifecycle::Output PowerLifecycle::pumpQueue() noexcept {
    if (m_phase != Phase::Idle || m_pending.empty()) return {};

    const auto next = m_pending.front();
    m_pending.pop_front();
    m_phaseSource = next.source;

    return next.kind == EventKind::Suspend
           ? makeSuspendEntry(next.source)
           : makeResumeEntry(next.source);
}

PowerLifecycle::Output
PowerLifecycle::makeSuspendEntry(Source /*source*/) noexcept {
    m_phase = Phase::Suspending;
    m_safetyFiredFirst = false;

    Output o;
    o.work        = Output::Work::StartSuspend;
    o.safetyTimer = Output::Timer::Arm;
    o.safetyDelay = std::chrono::duration_cast<std::chrono::milliseconds>(
        kSuspendSafetyDeadline);
    if (m_resumeRetryArmed) {
        // A pending post-resume retry is stale as soon as a new
        // suspend starts: the adapter is about to be closed.
        o.resumeRetry = Output::Timer::Disarm;
        m_resumeRetryArmed = false;
    }
    o.reconnectNotify = Output::Notify::SystemSuspend;
    return o;
}

PowerLifecycle::Output
PowerLifecycle::makeResumeEntry(Source /*source*/) noexcept {
    m_phase = Phase::Resuming;

    Output o;
    o.work = Output::Work::StartResume;
    if (m_resumeRetryArmed) {
        o.resumeRetry = Output::Timer::Disarm;
        m_resumeRetryArmed = false;
    }
    o.reconnectNotify = Output::Notify::SystemResume;
    return o;
}

PowerLifecycle::Output PowerLifecycle::onSuspendCompleted() noexcept {
    if (m_phase != Phase::Suspending) return {};

    const bool   firedFirst = m_safetyFiredFirst;
    const Source src        = m_phaseSource;

    m_phase = Phase::Idle;
    m_safetyFiredFirst = false;

    Output o;
    // Always disarm to match the historical unconditional disarm on
    // completion; TimerSource::disarm is idempotent when already disarmed.
    o.safetyTimer = Output::Timer::Disarm;
    if (firedFirst) {
        o.safetyOverrun = true;
    } else if (src == Source::DBus) {
        o.lock = Output::Lock::Release;
    }
    return o;
}

PowerLifecycle::Output
PowerLifecycle::onResumeCompleted(bool adapterValid) noexcept {
    if (m_phase != Phase::Resuming) return {};

    const Source src = m_phaseSource;
    m_phase = Phase::Idle;

    Output o;
    if (!adapterValid && !m_resumeRetryArmed) {
        // One delayed retry covers the window where the first reopen
        // races USB re-enumeration after wake. Suppressed if another
        // retry is already armed — a concurrent resume doesn't stack.
        o.resumeRetry = Output::Timer::Arm;
        o.resumeRetryDelay = std::chrono::duration_cast<std::chrono::milliseconds>(
            kResumeRetryDelay);
        m_resumeRetryArmed = true;
    }
    if (src == Source::DBus) o.lock = Output::Lock::Take;
    return o;
}

PowerLifecycle::Output PowerLifecycle::onSafetyTimerFired() noexcept {
    if (m_phase != Phase::Suspending || m_safetyFiredFirst) return {};
    m_safetyFiredFirst = true;

    Output o;
    o.safetyFired = true;
    if (m_phaseSource == Source::DBus) o.lock = Output::Lock::Release;
    return o;
}

PowerLifecycle::Output PowerLifecycle::onResumeRetryTimerFired() noexcept {
    if (m_phase != Phase::Idle || !m_resumeRetryArmed) return {};
    m_resumeRetryArmed = false;

    Output o;
    o.submitLateReconnect = true;
    return o;
}

PowerLifecycle::Output PowerLifecycle::onSafetyTimerArmFailed() noexcept {
    // Deliberate no-op. Leaving m_safetyFiredFirst false means the
    // next onSuspendCompleted takes the happy branch and releases the
    // lock — matching the behaviour of the single-flag predecessor
    // where the "safety armed" flag was never reset on arm failure.
    return {};
}

PowerLifecycle::Output PowerLifecycle::onResumeRetryArmFailed() noexcept {
    m_resumeRetryArmed = false;
    return {};
}

} // namespace cec_control
