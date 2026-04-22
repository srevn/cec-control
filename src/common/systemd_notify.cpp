#include "systemd_notify.h"

#include <systemd/sd-daemon.h>

#include <cstdint>

namespace cec_control {
namespace SystemdNotify {

void ready() noexcept {
    sd_notify(0, "READY=1");
}

void stopping() noexcept {
    sd_notify(0, "STOPPING=1");
}

void watchdog() noexcept {
    sd_notify(0, "WATCHDOG=1");
}

void status(std::string_view text) noexcept {
    // %.*s respects @p text.size() so the view need not be NUL-terminated.
    // A pathological payload larger than INT_MAX would truncate in the
    // cast, but no legitimate status string approaches that magnitude.
    sd_notifyf(0, "STATUS=%.*s",
               static_cast<int>(text.size()), text.data());
}

bool watchdogEnabled(std::chrono::microseconds& interval) noexcept {
    std::uint64_t usec = 0;
    const int rc = sd_watchdog_enabled(0, &usec);
    if (rc <= 0 || usec == 0) {
        return false;
    }
    interval = std::chrono::microseconds(
        static_cast<std::chrono::microseconds::rep>(usec));
    return true;
}

} // namespace SystemdNotify
} // namespace cec_control
