#pragma once

#include <string>
#include <memory>
#include <vector>
#include <optional>

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
     */
    CECClient();
    
    /**
     * Destructor
     */
    ~CECClient();
    
    /**
     * Process command line arguments
     * @param argc Argument count
     * @param argv Argument values
     * @return true if arguments were successfully processed
     */
    bool processArgs(int argc, char* argv[]);
    
    /**
     * Connect to daemon and execute command
     * @return Exit code (0 for success, non-zero for failure)
     */
    int execute();
    
    /**
     * Check if help was requested
     * @return true if help was requested
     */
    bool isHelpRequested() const { return m_printHelp; }

private:
    std::unique_ptr<SocketClient> m_socketClient;
    std::optional<Message> m_command;
    bool m_printHelp;
    std::string m_socketPath;
    
    // Parse command line arguments
    bool parseArgs(int argc, char* argv[]);
    
    // Connect to the daemon socket
    bool connect();
    
    // Print command result
    void printResult(const Message& response);
};

} // namespace cec_control