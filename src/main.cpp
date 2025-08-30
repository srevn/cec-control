#include "common/argument_parser.h"
#include "common/help_printer.h"
#include "common/logger.h"
#include "daemon/daemon_bootstrap.h"
#include "client/cec_client.h"

#include <iostream>
#include <cstdlib>

/**
 * Main entry point for the CEC control application
 */
int main(int argc, char* argv[]) {
    // Parse command line arguments
    auto parseResult = cec_control::ArgumentParser::parse(argc, argv);
    
    // Handle parsing errors
    if (parseResult.hasError) {
        std::cerr << parseResult.errorMessage << std::endl;
        return EXIT_FAILURE;
    }
    
    // Handle help requests
    if (parseResult.showHelp) {
        cec_control::HelpPrinter::printHelp(parseResult.mode, argv[0]);
        return EXIT_SUCCESS;
    }
    
    // Branch execution based on detected mode
    switch (parseResult.mode) {
        case cec_control::ApplicationMode::CLIENT: {
            // Configure minimal logging for client
            cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::ERROR);

            if (!parseResult.clientCommand.has_value()) {
                std::cerr << "Error: No valid command specified" << std::endl;
                return EXIT_FAILURE;
            }
            
            try {
                cec_control::CECClient client(parseResult.socketPath);
                return client.execute(parseResult.clientCommand.value());
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
                return EXIT_FAILURE;
            }
        }
            
        case cec_control::ApplicationMode::DAEMON:
            return cec_control::DaemonBootstrap::runDaemon(parseResult);
            
        case cec_control::ApplicationMode::HELP_GENERAL:
        case cec_control::ApplicationMode::HELP_CLIENT:
        case cec_control::ApplicationMode::HELP_DAEMON:
            // Help should have been handled above, but provide fallback
            cec_control::HelpPrinter::printHelp(parseResult.mode, argv[0]);
            return EXIT_SUCCESS;
            
        default:
            // Unknown mode - show general help
            cec_control::HelpPrinter::printHelp(cec_control::ApplicationMode::HELP_GENERAL, argv[0]);
            return EXIT_SUCCESS;
    }
}