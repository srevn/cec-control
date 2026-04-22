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
    /**
     * Outcome of a bootstrap step that may short-circuit @c runDaemon.
     *
     *  - @c ShouldRun             continue into the daemon loop.
     *  - @c ShouldReturnSuccess   parent-of-fork path; unwind
     *                             @c runDaemon cleanly and return
     *                             @c EXIT_SUCCESS.
     *  - @c ShouldReturnFailure   fork / setsid / setup failed;
     *                             unwind and return @c EXIT_FAILURE.
     *
     * Using @c return instead of @c exit() lets @c runDaemon's
     * stack-owned objects (@c ConfigManager, @c AppConfig, path
     * strings) destruct normally in the parent process — RAII-safe
     * against future resource additions.
     */
    enum class BootstrapPhase {
        ShouldRun,
        ShouldReturnSuccess,
        ShouldReturnFailure,
    };

    /** Setup the process (daemonization, service mode, etc.). */
    [[nodiscard]] static BootstrapPhase setupProcess(bool runAsDaemon);

    /** Daemonize the process by forking into background. */
    [[nodiscard]] static BootstrapPhase daemonize();

    /**
     * Initialize logging with the given configuration. @p logFile is the
     * resolved log path (already defaulted by the caller if @c action.logFile
     * was empty) and is used as-is without re-running the default-path
     * resolution.
     */
    static void setupLogging(const RunDaemon& action, const std::string& logFile);
};

} // namespace cec_control
