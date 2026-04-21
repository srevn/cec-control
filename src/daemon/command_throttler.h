#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

namespace cec_control {

/**
 * Tuning parameters for @c CommandThrottler. Consumed once at
 * construction and immutable thereafter. Defaults match the values
 * emitted by @c loadAppConfig when the operator omits every throttler
 * key, so a struct default-constructed in code behaves the same as
 * one loaded from a config-less install.
 */
struct ThrottlerConfig {
    uint32_t baseIntervalMs   = 200;
    uint32_t maxIntervalMs    = 1000;
    uint32_t maxRetryAttempts = 3;
};

/**
 * Adaptive inter-command back-off with exponential retry for the CEC
 * adapter.
 *
 * Thread-safe. Internal state (the next-allowed slot and the
 * consecutive-failure counter) lives in lock-free atomics, and every
 * sleep happens outside any lock — callers may invoke
 * @c executeWithThrottle from any thread without external
 * serialisation. Rate limiting is enforced via a CAS-based slot
 * reservation: concurrent callers queue on distinct slots rather than
 * racing for the same instant.
 *
 * This class fences only @e when each command may begin. Serialisation
 * of the underlying libcec call is the caller's responsibility, which
 * in practice happens inside @c LibCecAdapter via its adapter mutex.
 */
class CommandThrottler {
public:
    explicit CommandThrottler(ThrottlerConfig config);

    /**
     * Execute @p command under the throttle + retry policy. Returns
     * @c true on first success, @c false once every retry attempt has
     * been consumed. Thread-safe.
     */
    [[nodiscard]] bool executeWithThrottle(std::function<bool()> command);

private:
    using Clock     = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    const ThrottlerConfig m_config;

    // Earliest time at which the next command may begin. Each caller
    // CAS-reserves its slot by advancing this forward before sleeping,
    // so two concurrent callers cannot collapse onto the same instant.
    std::atomic<TimePoint> m_nextAllowed;

    // Consecutive-failure counter driving the adaptive inter-command
    // interval; see @c currentInterval.
    std::atomic<uint32_t> m_consecutiveFailures;

    /** Compute the current inter-command interval from the failure count. */
    [[nodiscard]] std::chrono::milliseconds currentInterval() const noexcept;

    /** Reserve the next throttle slot (CAS) and sleep until it lands. */
    void reserveSlotAndSleep();
};

} // namespace cec_control
