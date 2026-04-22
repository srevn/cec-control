#pragma once

#include <chrono>
#include <cstdint>

namespace cec_control {

/**
 * timerfd wrapper for one-shot deadlines in the unified event loop.
 *
 * Register the fd() with an EventLoop on READ; consume() inside the
 * handler returns the number of expirations (normally 1). armOnce()
 * replaces any prior arming; disarm() cancels. The timer clock is
 * CLOCK_MONOTONIC (suspend/wake robust in the sense that the monotonic
 * clock pauses across suspend; callers that want wall-clock semantics
 * would need CLOCK_BOOTTIME — none of our use sites do).
 */
class TimerSource {
public:
    TimerSource();
    ~TimerSource();

    TimerSource(const TimerSource&) = delete;
    TimerSource& operator=(const TimerSource&) = delete;

    /** True if the timerfd was created successfully. */
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    /** The fd to register with EventLoop (READ). */
    int fd() const noexcept { return m_fd; }

    /**
     * Arm the timer to fire once after @p d elapses. A prior arming is
     * overwritten. A duration of zero disarms (matches timerfd_settime
     * semantics). Returns false if the syscall fails.
     */
    [[nodiscard]] bool armOnce(std::chrono::milliseconds d);

    /**
     * Arm the timer to fire repeatedly every @p period. A prior arming
     * is replaced. A zero or negative period is a programming error
     * (would busy-spin or disarm ambiguously) and returns @c false
     * without touching the timer. Returns @c false on syscall failure.
     */
    [[nodiscard]] bool armPeriodic(std::chrono::milliseconds period);

    /** Cancel the current arming. Idempotent. */
    void disarm();

    /**
     * Drain the timerfd; returns the expiration count since the last
     * read (0 if the read would block, typically after a racy disarm+poll).
     */
    uint64_t consume() noexcept;

private:
    int m_fd = -1;
};

} // namespace cec_control
