#pragma once

#include "cec_daemon.h"
#include "../common/argument_parser.h"
#include "../common/config_manager.h"

namespace cec_control {

/**
 * Aggregate of everything DaemonBootstrap needs to hand into CECDaemon.
 * Keeps daemon-level lifecycle knobs and router-level CEC knobs on separate
 * members so CECDaemon's constructor takes one of each.
 */
struct DaemonAllOptions {
    CECDaemon::Options daemon;
    CommandRouter::Options router;
};

/**
 * Bootstrap class responsible for daemon process management and initialization
 * Extracts all process-level functionality from main.cpp
 */
class DaemonBootstrap {
public:
    /**
     * Initialize and run the CEC daemon described by @p action. Catches
     * std::exception so main() does not need to.
     * @return Exit code (EXIT_SUCCESS or EXIT_FAILURE)
     */
    static int runDaemon(const RunDaemon& action);

private:
    /** Setup the process (daemonization, service mode, etc.). */
    [[nodiscard]] static bool setupProcess(bool runAsDaemon);

    /** Check if running under systemd. */
    [[nodiscard]] static bool isRunningUnderSystemd();

    /** Daemonize the process by forking into background. */
    [[nodiscard]] static bool daemonize();

    /** Initialize logging with the given configuration. */
    static void setupLogging(const RunDaemon& action);

    /**
     * Read every tunable from @p cfg and package it into the option structs
     * that CECDaemon and CommandRouter consume. All config parsing — including
     * the comma-separated WakeDevices/PowerOffDevices lists — lives here.
     */
    static DaemonAllOptions loadAllOptions(const ConfigManager& cfg);
};

} // namespace cec_control
