#pragma once

#include "cec/adapter_config.h"
#include "command_throttler.h"

namespace cec_control {

class ConfigManager;

/**
 * Dispatcher-level policy flags. Each field is a seed value for a live
 * store on @c CommandDispatcher — @c autoStandbyEnabled in particular
 * is mirrored into an atomic there and toggled at runtime by
 * @c CMD_AUTO_STANDBY; this struct remains the file's original value.
 * See @c AppConfig's class comment for the snapshot-vs-live-state split.
 */
struct DispatcherConfig {
    bool queueCommandsDuringSuspend = true;
    bool autoStandbyEnabled         = false;
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
    DaemonConfig     daemon;
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
