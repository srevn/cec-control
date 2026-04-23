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

/**
 * @class CecHookSubsystem
 * @brief Main-thread subscriber of CEC bus observations that fires
 *        user scripts on three events: @c InputSwitch, @c TVStandby,
 *        @c TVWake.
 *
 * ## Threading
 *
 * Main-thread only. @c observe is invoked by the daemon's adapter-
 * observation forwarder after a @c MainThreadWork hop — the same path
 * @c StandbyPolicy uses. Cache state (@c m_lastPhysical,
 * @c m_lastTvPower) is therefore free of atomics or mutexes.
 *
 * Spawn work is handed off to @c HookExecutor; this class never
 * touches @c posix_spawn or any signal primitive directly.
 *
 * ## Dedup rule
 *
 * Dedup is per-event last-value, on the main thread:
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
 *  - @c ActiveSource → fire @c InputSwitch iff the physical address
 *    differs from the cached value (including the first observation,
 *    when the cache is @c nullopt); set cache to the new value.
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
 * The cache is about the TV's state, not the adapter's. On adapter
 * reopen (reconnect, post-resume late reconnect, @c CMD_RESTART_ADAPTER)
 * libcec re-enumerates the bus and TVs typically re-broadcast
 * @c ACTIVE_SOURCE and @c REPORT_POWER_STATUS. The dedup rule absorbs
 * those redundant broadcasts as long as the cache persists, so we do
 * not reset it and do not wire the subsystem into the suspend/resume
 * lifecycle at all.
 */
class CecHookSubsystem {
public:
    /**
     * @param config    Captured by value; frozen for the subsystem's
     *                  lifetime.
     * @param executor  Non-owning reference to the subsystem that will
     *                  spawn hook children. Must outlive this object;
     *                  enforced at the daemon level by destruction
     *                  order (@c m_hooks is reset before
     *                  @c m_hookExecutor).
     */
    CecHookSubsystem(HooksConfig config, HookExecutor& executor);

    CecHookSubsystem(const CecHookSubsystem&)            = delete;
    CecHookSubsystem& operator=(const CecHookSubsystem&) = delete;
    CecHookSubsystem(CecHookSubsystem&&)                 = delete;
    CecHookSubsystem& operator=(CecHookSubsystem&&)      = delete;

    /**
     * Consume one CEC observation. Applies the dedup rule above and,
     * if applicable, hands a prepared @c Job to the executor. Main
     * thread only.
     */
    void observe(const ICecAdapter::Observation& obs);

private:
    enum class CachedPower { Unknown, On, Standby };

    void fireInputSwitch(uint16_t newAddr);
    void fireTvStandby();
    void fireTvWake();

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
    const int           m_daemonPid;

    std::optional<uint16_t> m_lastPhysical;
    CachedPower             m_lastTvPower = CachedPower::Unknown;
};

} // namespace cec_control
