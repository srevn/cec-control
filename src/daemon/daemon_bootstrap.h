#pragma once

#include <string>

#include "../common/argument_parser.h"

namespace cec_control {

/**
 * Bootstrap class responsible for daemon process setup and initialisation.
 *
 * The daemon always runs in the foreground. Backgrounding is the
 * responsibility of the supervisor (systemd, systemd-run, shell @c &,
 * another init system) — the binary itself does not fork. This keeps
 * service unit and binary from having to co-negotiate who detaches.
 */
class DaemonBootstrap {
public:
    /**
     * Initialise and run the CEC daemon described by @p action. Catches
     * @c std::exception so @c main does not need to.
     * @return Exit code (@c EXIT_SUCCESS or @c EXIT_FAILURE).
     */
    static int runDaemon(const RunDaemon& action);

private:
    /**
     * Minimal process setup: redirect stdin to @c /dev/null so the
     * daemon never reads from an inherited terminal. Supervisors
     * typically wire @c StandardInput=null for us; this is a safety
     * net for non-supervised manual invocations.
     */
    static void setupProcess();

    /**
     * Initialize logging with the given configuration. @p logFile is the
     * resolved log path (already defaulted by the caller if @c action.logFile
     * was empty) and is used as-is without re-running the default-path
     * resolution.
     */
    static void setupLogging(const RunDaemon& action, const std::string& logFile);
};

} // namespace cec_control
