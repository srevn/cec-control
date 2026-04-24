#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../app_config.h"            // HooksConfig
#include "../cec/adapter_interface.h" // ICecAdapter::Observation

namespace cec_control {

class HookExecutor;
class TimerSource;

/**
 * @class CecHookSubsystem
 * @brief Main-thread subscriber of CEC bus observations that fires
 *        user scripts on three events: @c InputSwitch, @c TVStandby,
 *        @c TVWake.
 *
 * ## Threading
 *
 * Main-thread only. @c observe and @c onDebounceTimerFired are both
 * invoked on the event loop — the former by the daemon's adapter-
 * observation forwarder after a @c MainThreadWork hop (the same path
 * @c StandbyPolicy uses), the latter by the loop's handler for the
 * subsystem's own timerfd. Cache state (@c m_pendingPhysical,
 * @c m_lastFiredPhysical, @c m_lastTvPower) is therefore free of
 * atomics or mutexes.
 *
 * Spawn work is handed off to @c HookExecutor; this class never
 * touches @c posix_spawn or any signal primitive directly.
 *
 * ## Dedup rule
 *
 * Dedup is per-event last-value, on the main thread. The @c TvStandby
 * and @c TvPowerReport paths fire synchronously inline; the
 * @c ActiveSource path runs through a short debounce timer so that
 * startup bursts and AVR ping-pong sequences — where the bus emits
 * several @c ACTIVE_SOURCE / @c ROUTING_CHANGE / @c SET_STREAM_PATH
 * frames within a few milliseconds — collapse to a single fire on the
 * settled address.
 *
 *  - @c TvStandby opcode from TV → fire @c TVStandby iff cached TV
 *    power is not already @c Standby; set cache to @c Standby.
 *  - @c TvPowerReport with value @c ON → fire @c TVWake iff cached TV
 *    power is not already @c On; set cache to @c On.
 *  - Any other power-status value (STANDBY, intermediate transitions,
 *    @c UNKNOWN) DOES NOT touch cache. This is deliberate: @c TVStandby
 *    fires from the opcode only, so the cache must stay aligned with
 *    that rule — silently setting it to @c Standby on a report would
 *    dedup a subsequent legitimate @c STANDBY opcode.
 *  - @c ActiveSource → update the pending address and (re)arm the
 *    debounce timer, overwriting any prior arming. When the timer
 *    fires, @c onDebounceTimerFired compares the pending address
 *    against the last *fired* address; if they differ (including the
 *    first-ever commit, when the last-fired cache is @c nullopt) the
 *    hook fires and the cache advances. Startup bursts and transient
 *    bus "settling" traffic therefore collapse to a single fire on
 *    the final address, not one fire per observation.
 *
 * ## Config lifetime
 *
 * @c m_config is captured at construction and never mutated. A future
 * SIGHUP reload path must destroy and rebuild this subsystem rather
 * than patching in place — every dedup decision assumes the config is
 * frozen for the subsystem's lifetime.
 *
 * ## Why no suspend/resume awareness
 *
 * The caches are about the TV's state, not the adapter's. On adapter
 * reopen (reconnect, post-resume late reconnect, @c CMD_RESTART_ADAPTER)
 * libcec re-enumerates the bus and TVs typically re-broadcast
 * @c ACTIVE_SOURCE and @c REPORT_POWER_STATUS. The dedup rule absorbs
 * those redundant broadcasts as long as the caches persist, so we do
 * not reset them and do not wire the subsystem into the suspend/resume
 * lifecycle at all.
 */
class CecHookSubsystem {
public:
    /**
     * @param config          Captured by value; frozen for the
     *                        subsystem's lifetime.
     * @param executor        Non-owning reference to the subsystem
     *                        that will spawn hook children. Must
     *                        outlive this object; enforced at the
     *                        daemon level by destruction order
     *                        (@c m_hooks is reset before
     *                        @c m_hookExecutor).
     * @param debounceTimer   Non-owning reference to a timerfd
     *                        dedicated to this subsystem. The daemon
     *                        registers its fd with the event loop
     *                        and arranges for @c onDebounceTimerFired
     *                        to be called on expiry. Must outlive
     *                        this object.
     */
    CecHookSubsystem(HooksConfig config,
                     HookExecutor& executor,
                     TimerSource& debounceTimer);

    CecHookSubsystem(const CecHookSubsystem&)            = delete;
    CecHookSubsystem& operator=(const CecHookSubsystem&) = delete;
    CecHookSubsystem(CecHookSubsystem&&)                 = delete;
    CecHookSubsystem& operator=(CecHookSubsystem&&)      = delete;

    /**
     * Consume one CEC observation. Applies the dedup rule above and,
     * for @c ActiveSource, (re)arms the debounce timer; the other
     * kinds fire synchronously inline. Main thread only.
     */
    void observe(const ICecAdapter::Observation& obs);

    /**
     * Timer-fire entry point for the @c ActiveSource debounce. The
     * daemon's event-loop handler calls this when the configured
     * timerfd becomes readable. Commits the pending address if the
     * read reports a real expiration (see @c TimerSource::consume);
     * a zero expiration is treated as a racy supersede-then-read
     * and the commit is skipped — the fresh arming will commit its
     * own state when it fires. Main thread only.
     */
    void onDebounceTimerFired();

private:
    enum class CachedPower { Unknown, On, Standby };

    void fireInputSwitch(uint16_t newAddr);
    void fireTvStandby();
    void fireTvWake();

    /**
     * Commit the pending active-source address: fire @c InputSwitch
     * iff it differs from the last fired address; then advance the
     * last-fired cache to match. No-op when no observation has
     * updated the pending address since the last commit.
     */
    void commitPending();

    /**
     * Build the common base of the child environment: the sanitised
     * parent env (@c PATH, @c HOME, @c LANG, @c LC_ALL, @c USER if
     * set) plus @c CEC_EVENT, @c CEC_EVENT_TS, @c CEC_DAEMON_PID.
     * Each entry is a @c "KEY=VALUE" string; the caller appends
     * per-event additions before submitting.
     */
    [[nodiscard]] std::vector<std::string> baseEnv(std::string_view eventName) const;

    /**
     * Submit the job if @p scriptPath is non-empty; no-op otherwise.
     * Non-const: spawning a child is a visible side effect on the
     * outside world even though no class member is mutated, so
     * marking this @c const would read wrong to a reviewer.
     */
    void submit(std::string_view eventName,
                const std::string& scriptPath,
                std::vector<std::string> env);

    /**
     * Textual form of a cached-power tag for the @c CEC_TV_POWER_PREVIOUS
     * env entry. @c Unknown maps to the empty string — the contract's
     * "no prior state" sentinel.
     */
    [[nodiscard]] static std::string_view previousPowerString(CachedPower prev) noexcept;

    HooksConfig         m_config;
    HookExecutor&       m_executor;
    TimerSource&        m_debounceTimer;
    const int           m_daemonPid;

    // Active-source state split across the debounce boundary:
    //
    //  - @c m_pendingPhysical is written by @c observe every time an
    //    @c ActiveSource observation arrives. It is the bus's latest
    //    reported active source; until the debounce timer fires, no
    //    hook is emitted.
    //  - @c m_lastFiredPhysical is written by @c commitPending when a
    //    differing address is promoted. It is the value scripts have
    //    actually seen and the basis for @c CEC_SOURCE_PREVIOUS_PHYSICAL.
    //
    // @c nullopt on either field means "never set yet". A first-ever
    // commit therefore always fires — promoting @c nullopt to the
    // pending address — matching the prior single-cache behaviour.
    std::optional<uint16_t> m_pendingPhysical;
    std::optional<uint16_t> m_lastFiredPhysical;
    CachedPower             m_lastTvPower = CachedPower::Unknown;
};

} // namespace cec_control
