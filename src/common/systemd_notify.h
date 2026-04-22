#pragma once

#include <chrono>
#include <string_view>

namespace cec_control {
namespace SystemdNotify {

/**
 * Wrappers around libsystemd's service notification protocol. Every
 * entry point is a silent no-op when @c $NOTIFY_SOCKET is unset: the
 * daemon always invokes them unconditionally and the wrapper layer
 * hides the supervisor-agnostic safety net from the call sites.
 *
 * There is no "are we under systemd?" probe anywhere in this header —
 * the presence of @c $NOTIFY_SOCKET is the one load-bearing bit and
 * libsystemd consults it internally. A future alternative supervisor
 * that speaks the same READY/STOPPING/WATCHDOG protocol will be
 * picked up without code changes here.
 */

/**
 * Announce service readiness. Transitions a @c Type=notify unit from
 * @c activating to @c active and releases any @c After= dependents.
 * Must be sent only after every subsystem is wired and its fds are
 * registered, so that a dependent unit observes us as truly ready
 * and not merely executing.
 */
void ready() noexcept;

/**
 * Announce clean shutdown. Lets the service manager distinguish a
 * graceful stop from an unexpected exit for @c Restart= policy.
 * Fire-and-forget: subsequent @c READY=1 would not reverse it.
 */
void stopping() noexcept;

/**
 * Ping the service manager's watchdog. Must fire strictly below the
 * configured @c WatchdogSec interval; conventionally at half of it.
 * Missing a ping causes the service manager to treat the unit as
 * hung and apply its restart policy.
 */
void watchdog() noexcept;

/**
 * Emit a @c STATUS=<text> line for @c systemctl @c status. Allocation-
 * free (formatted in place via @c sd_notifyf). @p text need not be
 * NUL-terminated; overly long payloads are silently truncated by the
 * service manager.
 */
void status(std::string_view text) noexcept;

/**
 * If the service manager configured a watchdog for this unit, set
 * @p interval to the full @c WatchdogSec value and return @c true.
 * Callers arming a repeating timer should halve this value to
 * preserve the recommended ping cadence. Returns @c false when no
 * watchdog is configured, when the service manager is not listening,
 * or when this process is not the unit's notify PID.
 */
[[nodiscard]] bool watchdogEnabled(std::chrono::microseconds& interval) noexcept;

} // namespace SystemdNotify
} // namespace cec_control
