#include "cec_client.h"
#include "../common/logger.h"

#include <iostream>
#include <cstdlib>
#include <utility>

namespace cec_control {

CECClient::CECClient(std::string socketPath)
    : m_socketPath(std::move(socketPath)) {
}

CECClient::~CECClient() {
    // Socket client is automatically closed in its destructor
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

int CECClient::execute(const Message& command) {
    // Connect to daemon
    if (!connect()) {
        return EXIT_FAILURE;
    }
    
    // Send command
    Message response = m_socketClient->sendCommand(command);
    
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
