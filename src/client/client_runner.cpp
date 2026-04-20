#include "client_runner.h"

#include "cec_client.h"

#include <cstdlib>
#include <exception>
#include <iostream>

namespace cec_control {

int ClientRunner::run(const RunClient& action) {
    // The logger keeps its silent default on the client path. Every
    // diagnostic the client surfaces is rendered through CECClient onto
    // stderr; the absence of stray LOG_* lines on stdout is what lets
    // downstream pipelines parse the success line cleanly.

    try {
        CECClient client(action.socketPathOverride);
        return client.execute(action.command);
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}

} // namespace cec_control
