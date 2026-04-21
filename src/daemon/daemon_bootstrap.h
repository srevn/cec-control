#pragma once

#include "../common/argument_parser.h"

namespace cec_control {

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
};

} // namespace cec_control
