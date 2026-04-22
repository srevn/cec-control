#include "power_supervisor.h"

#include <chrono>

#include "../../common/logger.h"
#include "../../common/main_thread_work.h"
#include "../../common/timer_source.h"
#include "../cec/adapter_worker.h"
#include "../command_router.h"
#include "../dbus_monitor.h"

namespace cec_control {

PowerSupervisor::PowerSupervisor(CommandRouter&  router,
                                 AdapterWorker&  worker,
                                 MainThreadWork& work,
                                 TimerSource&    suspendSafety,
                                 TimerSource&    resumeRetry,
                                 TimerSource&    reconnectRetry) noexcept
    : m_router(router),
      m_worker(worker),
      m_work(work),
      m_suspendSafetyTimer(suspendSafety),
      m_resumeRetryTimer(resumeRetry),
      m_reconnectRetryTimer(reconnectRetry) {
    (void)m_work; // Held for parity with the daemon's wiring and for
                  // future cross-thread post needs (P2.5 unifies the
                  // reconnect paths through the supervisor); current
                  // code paths do not invoke it directly.
}

void PowerSupervisor::setDBusMonitor(DBusMonitor* dbusMonitor) noexcept {
    m_dbusMonitor = dbusMonitor;
}

void PowerSupervisor::onSuspendRequested(PowerLifecycle::Source source) {
    applyLifecycle(m_lifecycle.onSuspendRequested(source));
}

void PowerSupervisor::onResumeRequested(PowerLifecycle::Source source) {
    applyLifecycle(m_lifecycle.onResumeRequested(source));
}

void PowerSupervisor::onSuspendCompleted(std::chrono::milliseconds workDuration) {
    // Whichever path (completion vs. safety timer) fires first
    // discharges the inhibit-lock release; the other path takes the
    // overrun branch.
    auto out = m_lifecycle.onSuspendCompleted();
    if (out.safety == PowerLifecycle::Output::SafetyOutcome::Overrun) {
        LOG_WARNING("CEC suspend completed in ", workDuration.count(),
                    "ms but safety timer had already released the inhibit lock");
    } else {
        LOG_INFO("CEC suspend took ", workDuration.count(), "ms");
        if (out.lock == PowerLifecycle::Output::Lock::Release) {
            LOG_INFO("CEC sleep preparation complete; allowing system to sleep");
        }
    }
    applyLifecycle(out);
}

void PowerSupervisor::onResumeCompleted(bool adapterValid) {
    // After resume, the USB subsystem can still be settling. The FSM
    // emits a Timer::Arm for one delayed retry (gated on !adapterValid
    // and not-already-armed); the lock retake happens for DBus sources.
    applyLifecycle(m_lifecycle.onResumeCompleted(adapterValid));
}

void PowerSupervisor::onSafetyTimerFired() {
    m_suspendSafetyTimer.consume();
    auto out = m_lifecycle.onSafetyTimerFired();
    if (out.safety == PowerLifecycle::Output::SafetyOutcome::Fired) {
        LOG_WARNING("Suspend did not complete within ",
                    PowerLifecycle::kSuspendSafetyDeadline.count(),
                    "s; releasing inhibit lock forcibly");
    }
    applyLifecycle(out);
}

void PowerSupervisor::onResumeRetryTimerFired() {
    m_resumeRetryTimer.consume();
    applyLifecycle(m_lifecycle.onResumeRetryTimerFired());
}

void PowerSupervisor::onConnectionLost() {
    LOG_WARNING("CEC connection lost, attempting to reconnect");
    execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::ConnectionLost));
}

void PowerSupervisor::onReconnectResult(bool ok) {
    // A result arriving after a SystemSuspend/Resume has already moved
    // the FSM to Idle is absorbed as a no-op; no state check needed.
    if (ok) LOG_INFO("Successfully reconnected to CEC adapter");
    execute(m_adapterReconnect.onEvent(
        ok ? AdapterReconnect::Event::AttemptSucceeded
           : AdapterReconnect::Event::AttemptFailed));
}

void PowerSupervisor::onReconnectRetryTimerFired() {
    m_reconnectRetryTimer.consume();
    execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::RetryTimerFired));
}

bool PowerSupervisor::isSuspended() const noexcept {
    return m_router.isSuspended();
}

void PowerSupervisor::applyLifecycle(PowerLifecycle::Output output) {
    executeEffects(output);
    executeEffects(m_lifecycle.pumpQueue());
}

void PowerSupervisor::executeEffects(const PowerLifecycle::Output& out) {
    using Timer  = PowerLifecycle::Output::Timer;
    using Work   = PowerLifecycle::Output::Work;
    using Lock   = PowerLifecycle::Output::Lock;
    using Notify = PowerLifecycle::Output::Notify;

    // Entry banner for a new cycle. Emitted before any other effect so
    // a concurrent timer-arm warning lands after "starting X" in the
    // log.
    switch (out.work) {
    case Work::StartSuspend:
        LOG_INFO("System suspending, preparing CEC adapter");
        break;
    case Work::StartResume:
        LOG_INFO("System resuming, reinitializing CEC adapter");
        break;
    case Work::None:
        break;
    }

    // Disarm stale timers first so a subsequent Arm on the same axis
    // starts from a clean slate. TimerSource::disarm is idempotent.
    if (out.safetyTimer == Timer::Disarm) m_suspendSafetyTimer.disarm();
    if (out.resumeRetry == Timer::Disarm) m_resumeRetryTimer.disarm();

    // Notify the reconnect FSM. Its effects are carried out inline by
    // execute(); a SystemSuspend/SystemResume there disarms the
    // reconnect retry timer if one was armed.
    switch (out.reconnectNotify) {
    case Notify::SystemSuspend:
        execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::SystemSuspend));
        break;
    case Notify::SystemResume:
        execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::SystemResume));
        break;
    case Notify::None:
        break;
    }

    // Arm new timers. On syscall failure, feed back to the FSM so its
    // internal armed flags stay accurate; the returned Output is
    // always inert for these feedback events, so no further dispatch
    // is needed.
    if (out.safetyTimer == Timer::Arm) {
        if (!m_suspendSafetyTimer.armOnce(out.safetyDelay)) {
            LOG_WARNING("Could not arm suspend-safety timer; "
                        "inhibit lock will only release on completion");
            (void)m_lifecycle.onSafetyTimerArmFailed();
        }
    }
    if (out.resumeRetry == Timer::Arm) {
        if (!m_resumeRetryTimer.armOnce(out.resumeRetryDelay)) {
            LOG_WARNING("Could not arm resume-retry timer");
            (void)m_lifecycle.onResumeRetryArmFailed();
        }
    }

    // Inhibit-lock operations. m_dbusMonitor may be absent when power
    // monitoring is disabled by configuration or failed to initialise;
    // lock ops are no-ops in that case.
    if (m_dbusMonitor) {
        switch (out.lock) {
        case Lock::Release: m_dbusMonitor->releaseInhibitLock(); break;
        case Lock::Take:    m_dbusMonitor->takeInhibitLock();    break;
        case Lock::None:    break;
        }
    }

    // Adapter-side work.
    switch (out.work) {
    case Work::StartSuspend: submitSuspendWork(); break;
    case Work::StartResume:  submitResumeWork();  break;
    case Work::None:         break;
    }
    if (out.submitLateReconnect) submitLateReconnect();
}

void PowerSupervisor::submitSuspendWork() {
    // The router holds its own suspended/shutdown gates and may invoke
    // onDone synchronously on the early-return paths; the lifecycle
    // FSM's coalescing keeps that recursion shallow (typically one
    // step) and bounded by the queue depth. No defensive m_work hop is
    // needed: the supervisor's reference to the router is valid for
    // the supervisor's entire lifetime.
    m_router.suspendAsync([this](std::chrono::milliseconds elapsed) {
        this->onSuspendCompleted(elapsed);
    });
}

void PowerSupervisor::submitResumeWork() {
    m_router.resumeAsync([this](bool adapterValid) {
        this->onResumeCompleted(adapterValid);
    });
}

void PowerSupervisor::submitLateReconnect() {
    if (m_worker.isAdapterConnected() || m_router.isSuspended()) return;
    LOG_INFO("Performing delayed reconnection attempt");
    m_router.reconnectAsync(/*onDone*/ {});
}

void PowerSupervisor::submitReconnectAttempt() {
    m_router.reconnectAsync([this](bool ok) {
        this->onReconnectResult(ok);
    });
}

void PowerSupervisor::execute(AdapterReconnect::Output out) {
    using E = AdapterReconnect::Effect;
    switch (out.effect) {
    case E::None:
        break;
    case E::StartAttempt:
        // StartAttempt supersedes any armed retry timer; disarm()
        // unconditionally so the dispatcher stays free of state checks.
        m_reconnectRetryTimer.disarm();
        if (out.attemptNumber > 1) {
            LOG_INFO("Performing retried CEC reconnection (attempt ",
                     out.attemptNumber, "/", out.totalAttempts, ")");
        }
        submitReconnectAttempt();
        break;
    case E::ScheduleRetry:
        LOG_WARNING("CEC reconnect attempt failed; next retry in ",
                    out.delay.count(), "ms (attempt ",
                    out.attemptNumber, "/", out.totalAttempts, ")");
        if (!m_reconnectRetryTimer.armOnce(out.delay)) {
            LOG_ERROR("Failed to arm reconnect-retry timer");
            execute(m_adapterReconnect.onEvent(
                AdapterReconnect::Event::TimerArmFailed));
        }
        break;
    case E::CancelRetry:
        m_reconnectRetryTimer.disarm();
        break;
    case E::AbandonCycle:
        LOG_WARNING("CEC reconnect abandoned after ", out.totalAttempts,
                    " attempts; waiting for next connection-lost event");
        break;
    }
}

} // namespace cec_control
