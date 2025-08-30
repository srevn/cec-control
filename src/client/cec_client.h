#pragma once

#include <string>
#include <memory>
#include <utility>

#include "socket_client.h"
#include "../common/messages.h"

namespace cec_control {

/**
 * CEC client application class
 */
class CECClient {
public:
    /**
     * Create a new CEC client
     * @param socketPath Path to the daemon socket
     */
    explicit CECClient(std::string socketPath);
    
    /**
     * Destructor
     */
    ~CECClient();
    
    /**
     * Connect to daemon and execute a command
     * @param command The command to execute
     * @return Exit code (0 for success, non-zero for failure)
     */
    int execute(const Message& command);

private:
    std::unique_ptr<SocketClient> m_socketClient;
    std::string m_socketPath;
    
    // Connect to the daemon socket
    bool connect();
    
    // Print command result
    void printResult(const Message& response);
};

} // namespace cec_control