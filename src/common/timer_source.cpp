#include "timer_source.h"
#include "logger.h"

#include <sys/timerfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace cec_control {

TimerSource::TimerSource() {
    m_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (m_fd < 0) {
        LOG_ERROR("timerfd_create() failed: ", std::strerror(errno));
    }
}

TimerSource::~TimerSource() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool TimerSource::armOnce(std::chrono::milliseconds d) {
    if (m_fd < 0) return false;

    struct itimerspec spec{};
    const auto ms = d.count();
    // it_value {0,0} disarms per timerfd_settime(2); arm zero-duration
    // requests as a 1ns wake to preserve the "fire once" intent.
    if (ms > 0) {
        spec.it_value.tv_sec  = static_cast<time_t>(ms / 1000);
        spec.it_value.tv_nsec = static_cast<long>((ms % 1000) * 1'000'000);
    } else if (ms == 0) {
        spec.it_value.tv_nsec = 1;
    } else {
        // Negative durations are invalid; treat as immediate disarm.
        disarm();
        return false;
    }

    if (::timerfd_settime(m_fd, 0, &spec, nullptr) < 0) {
        LOG_ERROR("timerfd_settime(arm) failed: ", std::strerror(errno));
        return false;
    }
    return true;
}

bool TimerSource::armPeriodic(std::chrono::milliseconds period) {
    if (m_fd < 0 || period.count() <= 0) {
        return false;
    }

    struct itimerspec spec{};
    const auto ms = period.count();
    spec.it_value.tv_sec  = static_cast<time_t>(ms / 1000);
    spec.it_value.tv_nsec = static_cast<long>((ms % 1000) * 1'000'000);
    // Repeating semantics: kernel re-arms it_interval after each fire.
    spec.it_interval = spec.it_value;

    if (::timerfd_settime(m_fd, 0, &spec, nullptr) < 0) {
        LOG_ERROR("timerfd_settime(periodic) failed: ", std::strerror(errno));
        return false;
    }
    return true;
}

void TimerSource::disarm() {
    if (m_fd < 0) return;
    struct itimerspec spec{};  // all-zero disarms
    if (::timerfd_settime(m_fd, 0, &spec, nullptr) < 0) {
        LOG_WARNING("timerfd_settime(disarm) failed: ", std::strerror(errno));
    }
}

uint64_t TimerSource::consume() noexcept {
    if (m_fd < 0) return 0;
    uint64_t expirations = 0;
    ssize_t n = ::read(m_fd, &expirations, sizeof(expirations));
    if (n != static_cast<ssize_t>(sizeof(expirations))) {
        return 0;  // EAGAIN or partial read — caller treats as "no expiry".
    }
    return expirations;
}

} // namespace cec_control
