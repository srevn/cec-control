#include "command_throttler.h"
#include "../common/logger.h"

#include <algorithm>
#include <thread>

namespace cec_control {

namespace {

// Unit step for the failure-driven exponential schedules in both
// executeWithThrottle's post-attempt retry delay and currentInterval's
// extra term. Keeping them shared names the coupling: changing this
// value bumps both schedules in lockstep. Operator-tunable knobs for
// the adaptive interval live in ThrottlerConfig; this step is a
// source-level tuning parameter, not a config field.
constexpr uint32_t kFailureStepMs = 100;

} // namespace

CommandThrottler::CommandThrottler(ThrottlerConfig config)
    : m_config(config),
      m_nextAllowed(Clock::now()),
      m_consecutiveFailures(0) {}

bool CommandThrottler::executeWithThrottle(std::function<bool()> command) {
    for (uint32_t attempt = 0; attempt < m_config.maxRetryAttempts; ++attempt) {
        reserveSlotAndSleep();

        if (command()) {
            m_consecutiveFailures.store(0, std::memory_order_release);
            return true;
        }

        m_consecutiveFailures.fetch_add(1, std::memory_order_acq_rel);

        LOG_WARNING("CEC command failed, retry attempt ", attempt + 1,
                    " of ", m_config.maxRetryAttempts);

        // Exponential retry back-off, outside every lock so unrelated
        // callers progress freely.
        const uint32_t delayMs = (attempt == 0)
            ? kFailureStepMs
            : (kFailureStepMs * (1u << attempt));
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    LOG_INFO("Command sent but no successful acknowledgment received");

    // Soften the failure count by one: a retry-exhausted command is
    // treated as a single adaptive-throttle hit rather than N, matching
    // the pre-atomic semantics.
    uint32_t expected = m_consecutiveFailures.load(std::memory_order_relaxed);
    while (expected > 0 &&
           !m_consecutiveFailures.compare_exchange_weak(
               expected, expected - 1,
               std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // expected refreshed by failed CAS; loop and retry.
    }

    return false;
}

std::chrono::milliseconds CommandThrottler::currentInterval() const noexcept {
    const uint32_t failures = m_consecutiveFailures.load(std::memory_order_acquire);
    if (failures == 0) {
        return std::chrono::milliseconds(m_config.baseIntervalMs);
    }

    // Exponential back-off, capped at the span between base and max.
    const uint32_t capped = std::min(failures, 5u);
    const uint32_t span   = (m_config.maxIntervalMs > m_config.baseIntervalMs)
                            ? m_config.maxIntervalMs - m_config.baseIntervalMs
                            : 0u;
    const uint32_t extra  = std::min(kFailureStepMs * (1u << capped), span);
    return std::chrono::milliseconds(m_config.baseIntervalMs + extra);
}

void CommandThrottler::reserveSlotAndSleep() {
    const auto interval = currentInterval();
    const auto now      = Clock::now();

    TimePoint expected = m_nextAllowed.load(std::memory_order_acquire);
    TimePoint mySlot;
    for (;;) {
        // My slot is whichever is later: the next-reserved instant or now.
        mySlot = (expected > now) ? expected : now;
        const TimePoint nextReservation = mySlot + interval;
        if (m_nextAllowed.compare_exchange_weak(
                expected, nextReservation,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
        // expected refreshed by failed CAS; loop and re-evaluate mySlot.
    }

    if (mySlot > now) {
        const auto sleepFor = std::chrono::duration_cast<std::chrono::milliseconds>(
            mySlot - now);
        LOG_DEBUG("Throttling CEC command for ", sleepFor.count(),
                  "ms (adaptive delay)");
        std::this_thread::sleep_until(mySlot);
    }
}

} // namespace cec_control
