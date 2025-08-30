#include "common/argument_parser.h"
#include "common/help_printer.h"
#include "common/logger.h"
#include "daemon/daemon_bootstrap.h"
#include "client/socket_client.h"

#include <iostream>
#include <cstdlib>

/**
 * Execute client command using the parsed result
 * @param parseResult Parsed command line arguments
 * @return Exit code (EXIT_SUCCESS or EXIT_FAILURE)
 */
int runClient(const cec_control::ArgumentParser::ParseResult& parseResult) {
    // Configure minimal logging for client
    cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::ERROR);
    
    try {
        // Validate that we have a valid client command
        if (!parseResult.clientCommand.has_value()) {
            std::cerr << "Error: No valid command specified" << std::endl;
            return EXIT_FAILURE;
        }
        
        // Create socket client and connect to daemon
        cec_control::SocketClient socketClient(parseResult.socketPath);
        
        if (!socketClient.connect()) {
            std::cerr << "Error: Failed to connect to CEC daemon" << std::endl;
            std::cerr << "  Is the daemon running? Socket path: " << parseResult.socketPath << std::endl;
            return EXIT_FAILURE;
        }
        
        // Send command and get response
        cec_control::Message response = socketClient.sendCommand(parseResult.clientCommand.value());
        
        // Print result
        if (response.type == cec_control::MessageType::RESP_SUCCESS) {
            std::cout << "Command executed successfully" << std::endl;
            return EXIT_SUCCESS;
        } else {
            std::cerr << "Command failed" << std::endl;
            return EXIT_FAILURE;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}

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
        case cec_control::ApplicationMode::CLIENT:
            return runClient(parseResult);
            
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