#include "client/client_runner.h"
#include "common/argument_parser.h"
#include "common/help_printer.h"
#include "daemon/daemon_bootstrap.h"

#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <variant>

/**
 * Entry point: parse argv, then dispatch on the resulting Action variant.
 * Each runner (ClientRunner / DaemonBootstrap) catches its own exceptions
 * and returns an exit code; main holds no state and never throws.
 */
int main(int argc, char* argv[]) {
    using namespace cec_control;

    const Action action = ArgumentParser::parse(argc, argv);
    const char* programName = argv[0];

    return std::visit([programName](auto&& a) -> int {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, ParseError>) {
            std::cerr << a.message << '\n';
            return EXIT_FAILURE;
        } else if constexpr (std::is_same_v<T, ShowHelp>) {
            HelpPrinter::printHelp(a.target, programName);
            return EXIT_SUCCESS;
        } else if constexpr (std::is_same_v<T, RunClient>) {
            return ClientRunner::run(a);
        } else if constexpr (std::is_same_v<T, RunDaemon>) {
            return DaemonBootstrap::runDaemon(a);
        }
    }, action);
}
