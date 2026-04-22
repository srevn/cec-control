#pragma once

#include <libcec/cec.h>
#include <string>

namespace cec_control {

/**
 * Startup configuration for the libcec-backed adapter. Consumed once
 * at construction and immutable thereafter; runtime-mutable policy
 * (auto-standby) lives on @c CommandDispatcher.
 *
 * Lives in @c daemon/cec/ rather than @c common/ because the
 * @c CEC::cec_logical_addresses members transitively pull in
 * @c <libcec/cec.h> — a dependency the client deliberately does not
 * take on.
 */
struct AdapterConfig {
    std::string deviceName      = "CEC Controller";
    bool        autoPowerOn     = false;
    bool        autoWakeAVR     = false;
    bool        activateSource  = false;
    bool        systemAudioMode = false;
    CEC::cec_logical_addresses wakeDevices;
    CEC::cec_logical_addresses powerOffDevices;

    AdapterConfig() noexcept {
        // libcec's cec_logical_addresses is not self-clearing on
        // default construction; initialise explicitly so a default
        // AdapterConfig is a usable value rather than a trap.
        wakeDevices.Clear();
        powerOffDevices.Clear();
    }
};

} // namespace cec_control
