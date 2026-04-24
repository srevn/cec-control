#pragma once

#include <string>

#include "cec/adapter_config.h"
#include "command_throttler.h"

namespace cec_control {

class ConfigManager;

/**
 * Dispatcher-level policy flags. See @c AppConfig's class comment for
 * the snapshot-vs-live-state split.
 */
struct DispatcherConfig {
    bool queueCommandsDuringSuspend = true;
};

/**
 * Seed for @c StandbyPolicy: whether to suspend the system when the TV
 * signals standby. Mirrored into a plain @c bool on the policy at
 * construction and toggled at runtime by @c CMD_AUTO_STANDBY — both
 * readers and writers are on the main thread, so no atomic is needed.
 * See @c AppConfig's class comment for the snapshot-vs-live-state split.
 */
struct StandbyConfig {
    bool enabled = false;
};

/**
 * Daemon-level toggles. Read once at startup by @c CECDaemon::start
 * to decide whether to scan devices and whether to bring up the
 * D-Bus power monitor.
 */
struct DaemonConfig {
    bool enablePowerMonitor   = true;
    bool scanDevicesAtStartup = false;
};

/**
 * User-script hook paths, one per CEC bus event surfaced to userland.
 * Each field is either an absolute executable path or empty; empty
 * disables the corresponding hook. Validated at parse time — relative
 * paths are disabled with a warning, missing @c X_OK is warned but
 * tolerated (the user may @c chmod+x without restarting).
 *
 * The absolute-path requirement exists because the child is launched
 * with a sanitised environment that does not mirror the daemon's
 * @c PATH layout (see @c CecHookSubsystem::baseEnv); resolving via
 * @c $PATH could silently pick up the wrong binary.
 */
struct HooksConfig {
    std::string inputSwitch;
    std::string tvStandby;
    std::string tvWake;
    std::string hostActivated;
    std::string hostDeactivated;
};

/**
 * Typed, read-only snapshot of the configuration file.
 *
 * Loaded once at startup via @c loadAppConfig, handed to consumers
 * at construction, and never mutated thereafter. Runtime-mutable
 * policy (e.g. the wire-toggleable auto-standby flag) lives on the
 * consuming subsystem, seeded from this snapshot at construction —
 * @c AppConfig is the file's mirror, not the live policy store.
 *
 * Every field is itself a typed sub-struct scoped to one consumer
 * (adapter / throttler / dispatcher / daemon). File-layout-to-struct-
 * layout is mapped by @c loadAppConfig; a reader who wants to know
 * which INI section populates a given field should read that.
 *
 * A future SIGHUP reload would re-run @c loadAppConfig to produce a
 * fresh @c AppConfig, diff against the stored snapshot, and call the
 * appropriate setter on the owning subsystem; the snapshot itself is
 * replaced wholesale, never patched in place.
 */
struct AppConfig {
    AdapterConfig    adapter;
    ThrottlerConfig  throttler;
    DispatcherConfig dispatcher;
    StandbyConfig    standby;
    DaemonConfig     daemon;
    HooksConfig      hooks;
};

/**
 * Parse @p cfg into a typed @c AppConfig.
 *
 * Pure with respect to value echoing: the only diagnostics emitted
 * here are parse-level warnings (malformed logical-address entries).
 * Per-field "Configuration: X = Y" summaries go through
 * @c logAppConfig so a future reload path can parse silently and only
 * log a diff.
 */
[[nodiscard]] AppConfig loadAppConfig(const ConfigManager& cfg);

/**
 * Emit an INFO-level summary of the values in @p config. Deliberately
 * split from @c loadAppConfig — see that function's doc-comment for
 * the reasoning.
 */
void logAppConfig(const AppConfig& config);

} // namespace cec_control
