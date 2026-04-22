#include "power_supervisor.h"

#include <chrono>
#include <utility>
#include <vector>

#include "../../common/logger.h"
#include "../../common/messages.h"
#include "../../common/timer_source.h"
#include "../adapter_lifecycle.h"
#include "../cec/adapter_worker.h"
#include "../command_dispatcher.h"
#include "../dbus_monitor.h"

namespace cec_control {

PowerSupervisor::PowerSupervisor(CommandDispatcher& dispatcher,
                                 AdapterLifecycle&  lifecycle,
                                 AdapterWorker&     worker,
                                 TimerSource&       suspendSafety,
                                 TimerSource&       reconnectRetry) noexcept
    : m_dispatcher(dispatcher),
      m_lifecycle(lifecycle),
      m_worker(worker),
      m_suspendSafetyTimer(suspendSafety),
      m_reconnectRetryTimer(reconnectRetry) {}

void PowerSupervisor::setDBusMonitor(DBusMonitor* dbusMonitor) noexcept {
    m_dbusMonitor = dbusMonitor;
}

void PowerSupervisor::onSuspendRequested(PowerLifecycle::Source source) {
    applyLifecycle(m_powerLifecycle.onSuspendRequested(source));
}

void PowerSupervisor::onResumeRequested(PowerLifecycle::Source source) {
    applyLifecycle(m_powerLifecycle.onResumeRequested(source));
}

void PowerSupervisor::onSuspendCompleted(std::chrono::milliseconds workDuration) {
    // Whichever path (completion vs. safety timer) fires first
    // discharges the inhibit-lock release; the other path takes the
    // overrun branch.
    auto out = m_powerLifecycle.onSuspendCompleted();
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
    // Apply the lifecycle FSM output first (Resuming → Idle, lock
    // retake on DBus sources). If the adapter is still disconnected
    // after the worker-side reopen, seed the reconnect FSM with a
    // delayed first attempt to let USB re-enumeration settle.
    // Subsequent failures fall through to AdapterReconnect's backoff
    // schedule — no second timer is involved.
    applyLifecycle(m_powerLifecycle.onResumeCompleted(adapterValid));
    if (adapterValid) return;

    LOG_INFO("CEC adapter not connected after resume; seeding reconnect cycle");
    execute(m_adapterReconnect.seedCycle(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            kPostResumeRetryDelay)));
}

void PowerSupervisor::onSafetyTimerFired() {
    m_suspendSafetyTimer.consume();
    auto out = m_powerLifecycle.onSafetyTimerFired();
    if (out.safety == PowerLifecycle::Output::SafetyOutcome::Fired) {
        LOG_WARNING("Suspend did not complete within ",
                    PowerLifecycle::kSuspendSafetyDeadline.count(),
                    "s; releasing inhibit lock forcibly");
    }
    applyLifecycle(out);
}

void PowerSupervisor::onConnectionLost() {
    LOG_WARNING("CEC connection lost, attempting to reconnect");
    execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::ConnectionLost));
}

void PowerSupervisor::onReconnectResult(bool ok) {
    // A result arriving after a SystemSuspend/Resume has already moved
    // the FSM to Idle is absorbed as a no-op; no state check needed.
    if (ok) {
        LOG_INFO("Successfully reconnected to CEC adapter");
    } else {
        LOG_WARNING("CEC reconnect attempt failed");
    }
    execute(m_adapterReconnect.onEvent(
        ok ? AdapterReconnect::Event::AttemptSucceeded
           : AdapterReconnect::Event::AttemptFailed));
}

void PowerSupervisor::onReconnectRetryTimerFired() {
    m_reconnectRetryTimer.consume();
    execute(m_adapterReconnect.onEvent(AdapterReconnect::Event::RetryTimerFired));
}

bool PowerSupervisor::isSuspended() const noexcept {
    return m_lifecycle.isSuspended();
}

void PowerSupervisor::applyLifecycle(PowerLifecycle::Output output) {
    executeEffects(output);
    executeEffects(m_powerLifecycle.pumpQueue());
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

    // Notify the reconnect FSM. Its effects are carried out inline by
    // execute(); a SystemSuspend/SystemResume there cancels any armed
    // reconnect retry, including a seeded post-resume retry still
    // waiting on its delay.
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
            (void)m_powerLifecycle.onSafetyTimerArmFailed();
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
}

void PowerSupervisor::submitSuspendWork() {
    // The lifecycle holds its own suspended/shutdown gates and may
    // invoke onDone synchronously on the early-return paths; the FSM's
    // coalescing keeps that recursion shallow (typically one step) and
    // bounded by the queue depth. No cross-thread hop is needed
    // here: both the worker-completion path and the early-return
    // synchronous path land on the main thread by construction, and
    // the supervisor's reference to the lifecycle is valid for the
    // supervisor's entire lifetime.
    m_lifecycle.suspendAsync([this](std::chrono::milliseconds elapsed) {
        this->onSuspendCompleted(elapsed);
    });
}

void PowerSupervisor::submitResumeWork() {
    // The lifecycle drains the suspend queue inside its completion
    // path and hands the drained vector back here together with the
    // adapter validity. Forward the drained commands to the dispatcher
    // first so the worker submissions land before the FSM transition
    // observes the resume completion — preserving the pre-refactor
    // ordering where replays were submitted before onDone fired.
    m_lifecycle.resumeAsync(
        [this](bool adapterValid, std::vector<Message> queued) {
            if (!queued.empty()) m_dispatcher.replay(std::move(queued));
            this->onResumeCompleted(adapterValid);
        });
}

void PowerSupervisor::submitReconnectAttempt() {
    m_lifecycle.reconnectAsync([this](bool ok) {
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
        // Uniform phrasing: this effect now serves both the seeded
        // first attempt (from onResumeCompleted) and the failure-driven
        // retries (from AttemptFailed). The "attempt failed" signal is
        // logged at onReconnectResult, not here; this message reports
        // only the scheduling itself.
        LOG_INFO("CEC reconnect scheduled in ", out.delay.count(),
                 "ms (attempt ", out.attemptNumber, "/",
                 out.totalAttempts, ")");
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
