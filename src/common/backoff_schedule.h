#pragma once

#include <chrono>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <vector>

namespace cec_control {

/**
 * A fixed retry schedule. Each call to @c nextDelay() advances one
 * step and returns the delay to wait before the next attempt along
 * with the 1-based position of that step within the schedule;
 * @c std::nullopt once every entry has been consumed. @c reset()
 * rewinds so the caller can replay the schedule on a fresh failure.
 *
 * Consolidates the "arm a timer, bump an attempt counter, give up
 * after N" bookkeeping that otherwise replicates across every retry
 * site. Callers cache the returned @c Attempt locally if they need
 * the position at a later point (e.g. a retry timer firing some time
 * after the arm); the schedule keeps no post-advance peek API so the
 * internal counter stays private.
 *
 * NOT thread-safe — every current user lives on the daemon's main
 * thread and serialises implicitly. Add an external mutex if you
 * consume the schedule from multiple threads.
 */
class BackoffSchedule {
public:
    /**
     * One step drawn from the schedule.
     *
     *  - @c delay  wait before the attempt fires.
     *  - @c index  1-based position of this step (1 for the first
     *              @c nextDelay() call after construction/@c reset()).
     *  - @c total  number of steps in the schedule (@c size()).
     */
    struct Attempt {
        std::chrono::milliseconds delay;
        std::size_t               index;
        std::size_t               total;
    };

    BackoffSchedule(std::initializer_list<std::chrono::milliseconds> delays)
        : m_delays(delays) {}

    /**
     * Advance one step and return the delay plus the 1-based attempt
     * index the caller is about to act on. Returns @c std::nullopt
     * once the schedule is exhausted; the caller should log, give
     * up, and @c reset() the schedule when the failure condition
     * recurs.
     */
    [[nodiscard]] std::optional<Attempt> nextDelay() noexcept {
        if (m_attempt >= m_delays.size()) return std::nullopt;
        const auto delay = m_delays[m_attempt];
        ++m_attempt;
        return Attempt{delay, m_attempt, m_delays.size()};
    }

    /** Rewind the attempt counter. Idempotent. */
    void reset() noexcept { m_attempt = 0; }

    /** Total steps in the schedule. */
    [[nodiscard]] std::size_t size() const noexcept { return m_delays.size(); }

private:
    std::vector<std::chrono::milliseconds> m_delays;
    std::size_t m_attempt = 0;
};

} // namespace cec_control
