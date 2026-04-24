#include "cec_hook_subsystem.h"

#include "../../common/logger.h"
#include "../../common/timer_source.h"
#include "hook_executor.h"

#include <libcec/cec.h>

#include <unistd.h>
#include <time.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace cec_control {

namespace {

// Window over which back-to-back @c ActiveSource observations
// collapse to a single @c InputSwitch fire. Sized to cover the
// startup burst (observations arrive sub-millisecond apart) and
// the AVR re-route ping-pong (seconds apart — out of scope, each
// end fires) without introducing user-visible latency for a real
// input switch (an AVR takes hundreds of milliseconds to respond
// anyway). Fixed at a compile-time constant: a config knob is
// easy to add if a real bus ever produces a wider burst, but
// absent a reported need the simpler shape wins.
constexpr auto kDebounceWindow = std::chrono::milliseconds(200);

/**
 * Render a 16-bit CEC physical address in its conventional dotted-
 * nibble form — @c 0x2000 → @c "2.0.0.0", @c 0x1200 → @c "1.2.0.0".
 * This is the notation users see in TV menus and libcec logs, so it
 * is the representation we put in front of hook scripts.
 */
std::string dottedPhysicalAddress(uint16_t addr) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  static_cast<unsigned>((addr >> 12) & 0xFu),
                  static_cast<unsigned>((addr >>  8) & 0xFu),
                  static_cast<unsigned>((addr >>  4) & 0xFu),
                  static_cast<unsigned>((addr      ) & 0xFu));
    return std::string{buf};
}

/** Render a 16-bit physical address as the raw hex constant. */
std::string rawPhysicalAddress(uint16_t addr) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "0x%04x", static_cast<unsigned>(addr));
    return std::string{buf};
}

/**
 * ISO-8601 timestamp, UTC, second resolution: @c 2026-04-23T12:34:56Z.
 * The @c CEC_EVENT_TS contract is documented against this format; do
 * not switch to local time or add sub-second precision without
 * coordinating the change through @c docs/configuration.md.
 */
std::string nowIso8601Utc() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm{};
    gmtime_r(&ts.tv_sec, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buf};
}

/**
 * Append @c "NAME=VALUE" to @p env for every @p name in @p names that
 * is set in the current process environment. Unset names are skipped
 * entirely — emitting @c "HOME=" with an empty value would confuse
 * shell scripts that use @c ${HOME:-/tmp} defaults.
 */
void propagateIfSet(std::vector<std::string>& env,
                    std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const char* value = std::getenv(name);
        if (value == nullptr) continue;
        std::string entry{name};
        entry.push_back('=');
        entry.append(value);
        env.push_back(std::move(entry));
    }
}

} // namespace

CecHookSubsystem::CecHookSubsystem(HooksConfig config,
                                    HookExecutor& executor,
                                    TimerSource& debounceTimer)
    : m_config(std::move(config)),
      m_executor(executor),
      m_debounceTimer(debounceTimer),
      m_daemonPid(::getpid()) {}

void CecHookSubsystem::observe(const ICecAdapter::Observation& obs) {
    using Kind = ICecAdapter::Observation::Kind;
    switch (obs.kind) {
    case Kind::TvStandby:
        if (m_lastTvPower == CachedPower::Standby) {
            LOG_DEBUG("Hook dedup: TV standby already cached; no fire");
            return;
        }
        fireTvStandby();
        m_lastTvPower = CachedPower::Standby;
        return;

    case Kind::TvPowerReport:
        // Design: fire TVWake only on an ON report. Other values
        // (STANDBY / TO_ON / TO_STANDBY / UNKNOWN) DO NOT touch the
        // cache — see class-level doc for why.
        if (obs.power != CEC::CEC_POWER_STATUS_ON) {
            return;
        }
        if (m_lastTvPower == CachedPower::On) {
            LOG_DEBUG("Hook dedup: TV already on; no wake fire");
            return;
        }
        fireTvWake();
        m_lastTvPower = CachedPower::On;
        return;

    case Kind::ActiveSource:
        m_pendingPhysical = obs.physicalAddress;
        LOG_DEBUG("Hook debounce: pending active source = ",
                  dottedPhysicalAddress(obs.physicalAddress),
                  ", timer (re)armed");
        if (!m_debounceTimer.armOnce(kDebounceWindow)) {
            // armOnce logs its own failure. Fall back to an
            // immediate commit so we never silently lose a
            // transition — the debounce is a convenience, not a
            // correctness requirement.
            LOG_WARNING("Hook debounce timer arm failed; committing synchronously");
            commitPending();
        }
        return;
    }
}

void CecHookSubsystem::onDebounceTimerFired() {
    // A zero expiration count means the read would have blocked —
    // the most likely cause is that @c armOnce was called after
    // the kernel flagged the fd ready but before this handler ran
    // (e.g. an observation closure draining from @c MainThreadWork
    // in the same epoll batch). @c timerfd_settime resets the
    // expiration count, so the handler sees a spurious ready fd
    // with nothing to commit; the freshly-armed timer will fire on
    // its own schedule and commit then.
    if (m_debounceTimer.consume() == 0) {
        LOG_DEBUG("Hook debounce: spurious wake (arming superseded); skipping commit");
        return;
    }
    commitPending();
}

void CecHookSubsystem::commitPending() {
    if (!m_pendingPhysical.has_value()) {
        // Normal when the timer fires after a shutdown/teardown
        // sequence cleared the pending state. Nothing to do.
        return;
    }
    const uint16_t committed = *m_pendingPhysical;
    if (m_lastFiredPhysical && *m_lastFiredPhysical == committed) {
        LOG_DEBUG("Hook debounce: active source settled unchanged (",
                  dottedPhysicalAddress(committed), ")");
        m_pendingPhysical.reset();
        return;
    }
    fireInputSwitch(committed);
    m_lastFiredPhysical = committed;
    m_pendingPhysical.reset();
}

void CecHookSubsystem::fireInputSwitch(uint16_t newAddr) {
    auto env = baseEnv("InputSwitch");
    env.push_back("CEC_SOURCE_PHYSICAL=" + dottedPhysicalAddress(newAddr));
    env.push_back("CEC_SOURCE_PHYSICAL_RAW=" + rawPhysicalAddress(newAddr));
    // First fire: no prior committed address, emit as empty string
    // (the documented "no prior state" sentinel).
    env.push_back("CEC_SOURCE_PREVIOUS_PHYSICAL=" +
                  (m_lastFiredPhysical
                       ? dottedPhysicalAddress(*m_lastFiredPhysical)
                       : std::string{}));
    submit("InputSwitch", m_config.inputSwitch, std::move(env));
}

void CecHookSubsystem::fireTvStandby() {
    auto env = baseEnv("TVStandby");
    env.push_back("CEC_TV_POWER=standby");
    env.push_back("CEC_TV_POWER_PREVIOUS=" +
                  std::string{previousPowerString(m_lastTvPower)});
    submit("TVStandby", m_config.tvStandby, std::move(env));
}

void CecHookSubsystem::fireTvWake() {
    auto env = baseEnv("TVWake");
    env.push_back("CEC_TV_POWER=on");
    env.push_back("CEC_TV_POWER_PREVIOUS=" +
                  std::string{previousPowerString(m_lastTvPower)});
    submit("TVWake", m_config.tvWake, std::move(env));
}

std::vector<std::string>
CecHookSubsystem::baseEnv(std::string_view eventName) const {
    std::vector<std::string> env;
    env.reserve(10);

    // Sanitised parent env — five keys that real-world shell scripts
    // commonly rely on. Anything else (TZ, DBUS_*, XDG_*, systemd-set
    // internals) is deliberately stripped: the env contract for hook
    // scripts should be small and predictable, not a reflection of
    // whatever the daemon happens to have inherited from its unit.
    propagateIfSet(env, {"PATH", "HOME", "LANG", "LC_ALL", "USER"});

    std::string eventEntry = "CEC_EVENT=";
    eventEntry.append(eventName);
    env.push_back(std::move(eventEntry));

    env.push_back("CEC_EVENT_TS=" + nowIso8601Utc());
    env.push_back("CEC_DAEMON_PID=" + std::to_string(m_daemonPid));
    return env;
}

void CecHookSubsystem::submit(std::string_view eventName,
                               const std::string& scriptPath,
                               std::vector<std::string> env) {
    if (scriptPath.empty()) {
        // Hook is disabled for this event; emit nothing — a DEBUG line
        // per bus event would flood the log with noise proportional to
        // bus chatter.
        return;
    }
    LOG_INFO("Firing hook: ", eventName, " -> ", scriptPath);
    HookExecutor::Job job;
    job.path = scriptPath;
    job.env  = std::move(env);
    m_executor.submit(std::move(job));
}

std::string_view CecHookSubsystem::previousPowerString(CachedPower prev) noexcept {
    switch (prev) {
    case CachedPower::On:      return "on";
    case CachedPower::Standby: return "standby";
    case CachedPower::Unknown: return "";
    }
    return "";  // unreachable; quiets -Wreturn-type on strict compilers
}

} // namespace cec_control
