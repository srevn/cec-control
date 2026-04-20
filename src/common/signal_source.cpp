#include "signal_source.h"
#include "logger.h"

#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace cec_control {

SignalSource::SignalSource(std::initializer_list<int> signals) {
    sigemptyset(&m_mask);
    for (int s : signals) {
        sigaddset(&m_mask, s);
    }

    // Block on the caller's thread before creating signalfd, so any
    // subsequently-spawned thread inherits the mask and defers delivery
    // to the fd on this thread.
    if (pthread_sigmask(SIG_BLOCK, &m_mask, nullptr) != 0) {
        LOG_ERROR("pthread_sigmask(SIG_BLOCK) failed: ", std::strerror(errno));
        return;
    }

    m_fd = ::signalfd(-1, &m_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (m_fd < 0) {
        LOG_ERROR("signalfd() failed: ", std::strerror(errno));
    }
}

SignalSource::~SignalSource() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

std::optional<signalfd_siginfo> SignalSource::readOne() noexcept {
    if (m_fd < 0) return std::nullopt;

    signalfd_siginfo info;
    ssize_t n = ::read(m_fd, &info, sizeof(info));
    if (n != static_cast<ssize_t>(sizeof(info))) {
        // EAGAIN => no more queued signals; any short read is treated as
        // end-of-batch. Other errors are rare and best-effort.
        return std::nullopt;
    }
    return info;
}

} // namespace cec_control
