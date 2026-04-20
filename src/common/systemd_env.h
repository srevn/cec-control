#pragma once

namespace cec_control {
namespace SystemdEnv {

/**
 * Returns true if this process was launched by systemd. Result is cached on
 * first call (environment cannot change mid-process without explicit action
 * from within this process, so caching is safe).
 *
 * The probe inspects three environment variables in order of reliability:
 *   - NOTIFY_SOCKET   — set by units with Type=notify/notify-reload
 *   - INVOCATION_ID   — set by systemd for every unit since v232
 *   - SYSTEMD_EXEC_PID — set by systemd for every unit since v245
 *
 * Any one present is sufficient; the last two cover units that don't request
 * readiness notification.
 */
bool isUnderSystemd() noexcept;

} // namespace SystemdEnv
} // namespace cec_control
