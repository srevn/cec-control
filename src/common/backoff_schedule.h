#pragma once

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <vector>

namespace cec_control {

/**
 * A fixed retry schedule. Each call to @c nextDelay() advances one step
 * and returns the delay to wait before the next attempt; @c std::nullopt
 * once every entry has been consumed. @c reset() rewinds so the caller
 * can replay the schedule on a fresh failure.
 *
 * Consolidates the "arm a timer, bump an attempt counter, give up after
 * N" bookkeeping that otherwise replicates across every retry site.
 *
 * NOT thread-safe — every current user lives on the daemon's main thread
 * and serialises implicitly. Add an external mutex if you consume the
 * schedule from multiple threads.
 */
class BackoffSchedule {
public:
    BackoffSchedule(std::initializer_list<std::chrono::milliseconds> delays)
        : m_delays(delays) {}

    /**
     * Advance one attempt and return the delay to wait before firing it.
     * Returns @c std::nullopt once the schedule is exhausted; the caller
     * should log, give up, and @c reset() the schedule when the failure
     * condition recurs.
     */
    [[nodiscard]] std::optional<std::chrono::milliseconds> nextDelay() noexcept {
        if (m_attempt >= m_delays.size()) return std::nullopt;
        return m_delays[m_attempt++];
    }

    /** Rewind the attempt counter. Idempotent. */
    void reset() noexcept { m_attempt = 0; }

    /**
     * How many entries have been drawn from the schedule since the last
     * @c reset(). Intended for diagnostics ("attempt N of M") — do not
     * use as the sole driver for give-up logic; @c nextDelay() is the
     * authoritative signal.
     */
    [[nodiscard]] std::size_t attemptsSoFar() const noexcept { return m_attempt; }

    /** Total attempts in the schedule. */
    [[nodiscard]] std::size_t size() const noexcept { return m_delays.size(); }

private:
    std::vector<std::chrono::milliseconds> m_delays;
    std::size_t m_attempt = 0;
};

} // namespace cec_control
