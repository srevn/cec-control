#pragma once

#include <chrono>
#include <limits>
#include <sys/time.h>

namespace cec_control {

/**
 * A monotonic-clock deadline that composes across blocking operations.
 *
 * Construct once at the start of a bounded operation, then thread the same
 * Deadline through each step (connect, send, recv...). Each step asks for
 * the remaining budget rather than resetting a fresh timeout each call.
 */
class Deadline {
public:
    using clock = std::chrono::steady_clock;

    static Deadline in(std::chrono::milliseconds duration) noexcept {
        return Deadline{clock::now() + duration};
    }

    static Deadline never() noexcept {
        return Deadline{clock::time_point::max()};
    }

    bool isUnbounded() const noexcept {
        return m_expiry == clock::time_point::max();
    }

    bool expired() const noexcept {
        return !isUnbounded() && clock::now() >= m_expiry;
    }

    std::chrono::milliseconds remaining() const noexcept {
        if (isUnbounded()) {
            return std::chrono::milliseconds{-1};
        }
        auto now = clock::now();
        if (now >= m_expiry) {
            return std::chrono::milliseconds{0};
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(m_expiry - now);
    }

    /** Returns -1 for unbounded, otherwise a non-negative poll()-compatible millisecond value. */
    int remainingMs() const noexcept {
        auto r = remaining();
        if (r.count() < 0) return -1;
        constexpr auto kMaxInt = std::numeric_limits<int>::max();
        if (r.count() > kMaxInt) return kMaxInt;
        return static_cast<int>(r.count());
    }

    /** Returns a timeval suitable for SO_RCVTIMEO / SO_SNDTIMEO. Zero means "no timeout" to the kernel. */
    struct timeval toTimeval() const noexcept {
        struct timeval tv{0, 0};
        auto r = remaining();
        if (r.count() > 0) {
            tv.tv_sec = static_cast<time_t>(r.count() / 1000);
            tv.tv_usec = static_cast<suseconds_t>((r.count() % 1000) * 1000);
        }
        return tv;
    }

private:
    explicit Deadline(clock::time_point expiry) noexcept : m_expiry(expiry) {}
    clock::time_point m_expiry;
};

} // namespace cec_control
