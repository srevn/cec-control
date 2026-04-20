#pragma once

#include "../common/argument_parser.h"

namespace cec_control {

/**
 * Top-level orchestration for the client invocation path. Mirrors
 * DaemonBootstrap::runDaemon for the client side: owns the lifetime of the
 * CECClient, decides where its diagnostics go, and translates any
 * exceptional condition into an exit code.
 *
 * Stateless: nothing persists between invocations.
 */
class ClientRunner {
public:
    /**
     * Execute the client command described by @p action and return a process
     * exit code. EXIT_SUCCESS only when the daemon acknowledged with
     * RESP_SUCCESS. Catches std::exception so main() does not need to.
     */
    static int run(const RunClient& action);
};

} // namespace cec_control
