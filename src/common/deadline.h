#pragma once

#include <chrono>
#include <limits>

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

    /** Returns -1 for unbounded, otherwise a non-negative poll()-compatible millisecond value. */
    int remainingMs() const noexcept {
        auto r = remaining();
        if (r.count() < 0) return -1;
        constexpr auto kMaxInt = std::numeric_limits<int>::max();
        if (r.count() > kMaxInt) return kMaxInt;
        return static_cast<int>(r.count());
    }

private:
    explicit Deadline(clock::time_point expiry) noexcept : m_expiry(expiry) {}

    bool isUnbounded() const noexcept {
        return m_expiry == clock::time_point::max();
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

    clock::time_point m_expiry;
};

} // namespace cec_control
