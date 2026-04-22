#include "app_config.h"

#include "../common/config_manager.h"
#include "../common/logger.h"

#include <libcec/cec.h>

#include <sstream>
#include <string>
#include <string_view>

namespace cec_control {

namespace {

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
}

} // namespace cec_control
