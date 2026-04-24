#include "app_config.h"

#include "../common/config_manager.h"
#include "../common/logger.h"

#include <libcec/cec.h>

#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace cec_control {

namespace {

/**
 * Validate a single hook-path string: non-empty, absolute, executable.
 * Returns the path unchanged if accepted; returns empty if the path
 * was non-empty but failed a hard gate (relative). A non-executable
 * absolute path is tolerated with a warning — the user may @c chmod+x
 * without restarting, and @c posix_spawn will surface any residual
 * failure at fire time.
 */
std::string validateHookPath(std::string path, std::string_view eventName) {
    if (path.empty()) return path;
    if (path.front() != '/') {
        LOG_WARNING("Hook path for ", eventName,
                    " must be absolute; disabling: ", path);
        return std::string{};
    }
    if (::access(path.c_str(), X_OK) != 0) {
        LOG_WARNING("Hook path for ", eventName,
                    " not executable at startup; will retry at spawn time: ",
                    std::strerror(errno), " (path=", path, ")");
    }
    return path;
}

/**
 * Parse a comma-separated list of CEC logical addresses (e.g. "0,1,5")
 * into a @c cec_logical_addresses bitmask. Out-of-range or non-numeric
 * entries are logged and skipped; an empty input yields a cleared
 * mask. No whitespace handling — matches the legacy behaviour and
 * keeps the config format predictable.
 */
CEC::cec_logical_addresses parseLogicalAddressList(const std::string& input,
                                                    std::string_view fieldLabel) {
    CEC::cec_logical_addresses addrs;
    addrs.Clear();
    if (input.empty()) return addrs;

    std::stringstream ss(input);
    std::string token;
    while (std::getline(ss, token, ',')) {
        try {
            int id = std::stoi(token);
            if (id >= 0 && id <= 15) {
                addrs.Set(static_cast<CEC::cec_logical_address>(id));
            } else {
                LOG_WARNING("Logical address out of range in config field ",
                            fieldLabel, ": ", id);
            }
        } catch (const std::exception&) {
            LOG_WARNING("Invalid logical address in config field ",
                        fieldLabel, ": ", token);
        }
    }
    return addrs;
}

} // namespace

AppConfig loadAppConfig(const ConfigManager& cfg) {
    AppConfig config;

    // Adapter section — libcec-owned knobs the daemon hands into
    // LibCecAdapter at construction.
    auto& adapter = config.adapter;
    adapter.deviceName      = cfg.getString("Adapter", "DeviceName", "CEC Controller");
    adapter.autoPowerOn     = cfg.getBool  ("Adapter", "AutoPowerOn", false);
    adapter.autoWakeAVR     = cfg.getBool  ("Adapter", "AutoWakeAVR", false);
    adapter.activateSource  = cfg.getBool  ("Adapter", "ActivateSource", false);
    adapter.systemAudioMode = cfg.getBool  ("Adapter", "SystemAudioMode", false);
    adapter.wakeDevices     = parseLogicalAddressList(
        cfg.getString("Adapter", "WakeDevices", ""), "WakeDevices");
    adapter.powerOffDevices = parseLogicalAddressList(
        cfg.getString("Adapter", "PowerOffDevices", ""), "PowerOffDevices");

    // Throttler section — tuning for CommandThrottler. Keep in sync
    // with the defaults on ThrottlerConfig.
    auto& throttler = config.throttler;
    throttler.baseIntervalMs   = cfg.getInt("Throttler", "BaseIntervalMs", 200);
    throttler.maxIntervalMs    = cfg.getInt("Throttler", "MaxIntervalMs", 1000);
    throttler.maxRetryAttempts = cfg.getInt("Throttler", "MaxRetryAttempts", 3);

    // Dispatcher policy. QueueCommandsDuringSuspend lives under
    // [Daemon] in the file for backwards compatibility with deployed
    // configs, but is a dispatcher-scoped flag.
    auto& dispatcher = config.dispatcher;
    dispatcher.queueCommandsDuringSuspend =
        cfg.getBool("Daemon", "QueueCommandsDuringSuspend", true);

    // Standby policy. PowerOffOnStandby lives under [Adapter]
    // historically — it names the CEC standby opcode the policy
    // responds to — but the policy itself is enforced by
    // StandbyPolicy, not libcec. See the LibCecAdapter constructor
    // for why bPowerOffOnStandby is deliberately not mirrored into
    // libcec's own config.
    auto& standby = config.standby;
    standby.enabled =
        cfg.getBool("Adapter", "PowerOffOnStandby", false);

    // Daemon-level toggles.
    auto& daemon = config.daemon;
    daemon.enablePowerMonitor =
        cfg.getBool("Daemon", "EnablePowerMonitor",   true);
    daemon.scanDevicesAtStartup =
        cfg.getBool("Daemon", "ScanDevicesAtStartup", false);

    // Hooks section — one absolute-path entry per event. Validate at
    // parse time: reject relative paths outright, warn (but keep) on
    // missing X_OK so operators can fix permissions without a restart.
    auto& hooks = config.hooks;
    hooks.inputSwitch = validateHookPath(
        cfg.getString("Hooks", "InputSwitch", ""), "InputSwitch");
    hooks.tvStandby = validateHookPath(
        cfg.getString("Hooks", "TVStandby", ""), "TVStandby");
    hooks.tvWake = validateHookPath(
        cfg.getString("Hooks", "TVWake", ""), "TVWake");
    hooks.hostActivated = validateHookPath(
        cfg.getString("Hooks", "HostActivated", ""), "HostActivated");
    hooks.hostDeactivated = validateHookPath(
        cfg.getString("Hooks", "HostDeactivated", ""), "HostDeactivated");

    // Warn on typos. The known-key set is the events above; any other
    // key in [Hooks] is silently ignored by the parser, which is a
    // usability footgun — a stray "TV-Wake" (hyphen) would look
    // correct and do nothing. One warning per unknown key.
    static constexpr std::array<std::string_view, 5> kKnownHookKeys{
        "InputSwitch", "TVStandby", "TVWake", "HostActivated", "HostDeactivated",
    };
    for (const auto& [key, value] : cfg.section("Hooks")) {
        const bool known =
            std::find(kKnownHookKeys.begin(), kKnownHookKeys.end(), key) !=
            kKnownHookKeys.end();
        if (!known) {
            LOG_WARNING("Unknown key in [Hooks]: ", key, " (ignored)");
        }
        (void)value;
    }

    return config;
}

void logAppConfig(const AppConfig& config) {
    LOG_INFO("Configuration: ScanDevicesAtStartup = ",
             (config.daemon.scanDevicesAtStartup ? "true" : "false"));
    LOG_INFO("Configuration: QueueCommandsDuringSuspend = ",
             (config.dispatcher.queueCommandsDuringSuspend ? "true" : "false"));
    LOG_INFO("Configuration: EnablePowerMonitor = ",
             (config.daemon.enablePowerMonitor ? "true" : "false"));
    LOG_INFO("Configuration: PowerOffOnStandby = ",
             (config.standby.enabled ? "true" : "false"));

    // Only surface configured hooks; a silent [Hooks] section should
    // not spam "= (empty)" lines into the operator's view.
    if (!config.hooks.inputSwitch.empty()) {
        LOG_INFO("Configuration: Hooks.InputSwitch = ", config.hooks.inputSwitch);
    }
    if (!config.hooks.tvStandby.empty()) {
        LOG_INFO("Configuration: Hooks.TVStandby = ", config.hooks.tvStandby);
    }
    if (!config.hooks.tvWake.empty()) {
        LOG_INFO("Configuration: Hooks.TVWake = ", config.hooks.tvWake);
    }
    if (!config.hooks.hostActivated.empty()) {
        LOG_INFO("Configuration: Hooks.HostActivated = ", config.hooks.hostActivated);
    }
    if (!config.hooks.hostDeactivated.empty()) {
        LOG_INFO("Configuration: Hooks.HostDeactivated = ", config.hooks.hostDeactivated);
    }
}

} // namespace cec_control
