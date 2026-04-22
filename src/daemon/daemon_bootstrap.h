#pragma once

#include <string>

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

    /** Daemonize the process by forking into background. */
    [[nodiscard]] static bool daemonize();

    /**
     * Initialize logging with the given configuration. @p logFile is the
     * resolved log path (already defaulted by the caller if @c action.logFile
     * was empty) and is used as-is without re-running the default-path
     * resolution.
     */
    static void setupLogging(const RunDaemon& action, const std::string& logFile);
};

} // namespace cec_control
