#include "cec_client.h"
#include "command_mapper.h"
#include "../common/logger.h"
#include "../common/system_paths.h"

#include <iostream>
#include <cstdlib>
#include <getopt.h>

namespace cec_control {

CECClient::CECClient()
    : m_printHelp(false),
      m_socketPath(SystemPaths::getSocketPath()) {
}

CECClient::~CECClient() {
    // Socket client is automatically closed in its destructor
}

bool CECClient::processArgs(int argc, char* argv[]) {
    // If no arguments provided, show help
    if (argc < 2) {
        m_printHelp = true;
        return true;
    }
    
    return parseArgs(argc, argv);
}

bool CECClient::parseArgs(int argc, char* argv[]) {
    // Parse first argument as command
    std::string command = argv[1];
    
    // Handle help command
    if (command == "help" || command == "--help" || command == "-h") {
        m_printHelp = true;
        return true;
    }
    
    // Handle socket path override
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.substr(0, 13) == "--socket-path=") {
            m_socketPath = arg.substr(13);
        }
    }
    
    // Parse command
    auto checkArgCount = [&](int expectedCount, const std::string& commandName) {
        if (argc < expectedCount) {
            std::cerr << "Error: " << commandName << " command requires " << expectedCount - 2 << " arguments(s)" << std::endl;
            return false;
        }
        return true;
    };

    if (command == "volume") {
        if (!checkArgCount(4, command)) return false;
        m_command = CommandMapper::mapVolumeCommand(argv[2], argv[3]);
    }
    else if (command == "power") {
        if (!checkArgCount(4, command)) return false;
        m_command = CommandMapper::mapPowerCommand(argv[2], argv[3]);
    }
    else if (command == "source") {
        if (!checkArgCount(4, command)) return false;
        m_command = CommandMapper::mapSourceCommand(argv[2], argv[3]);
    }
    else if (command == "auto-standby") {
        if (!checkArgCount(3, command)) return false;
        m_command = CommandMapper::mapAutoStandbyCommand(argv[2]);
    }
    else if (command == "restart") {
        m_command = CommandMapper::mapRestartCommand();
    }
    else if (command == "suspend") {
        m_command = CommandMapper::mapSuspendCommand();
    }
    else if (command == "resume") {
        m_command = CommandMapper::mapResumeCommand();
    }
    else {
        std::cerr << "Error: Unknown command: " << command << std::endl;
        return false;
    }

    if (!m_command.has_value()) {
        return false;
    }

    return true;
}

bool CECClient::connect() {
    m_socketClient = std::make_unique<SocketClient>(m_socketPath);
    
    if (!m_socketClient->connect()) {
        std::cerr << "Error: Failed to connect to CEC daemon" << std::endl;
        std::cerr << "  Is the daemon running? Socket path: " << m_socketPath << std::endl;
        return false;
    }
    
    return true;
}

int CECClient::execute() {
    // Check if help was requested
    if (m_printHelp) {
        return EXIT_SUCCESS;
    }
    
    // Validate command
    if (!m_command.has_value()) {
        std::cerr << "Error: No valid command specified" << std::endl;
        return EXIT_FAILURE;
    }
    
    // Connect to daemon
    if (!connect()) {
        return EXIT_FAILURE;
    }
    
    // Send command
    Message response = m_socketClient->sendCommand(m_command.value());
    
    // Print result
    printResult(response);
    
    // Return appropriate exit code
    return (response.type == MessageType::RESP_SUCCESS) ? EXIT_SUCCESS : EXIT_FAILURE;
}

void CECClient::printResult(const Message& response) {
    if (response.type == MessageType::RESP_SUCCESS) {
        std::cout << "Command executed successfully" << std::endl;
    } else {
        std::cerr << "Command failed" << std::endl;
    }
}

} // namespace cec_control
