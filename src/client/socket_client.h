#pragma once

#include <string>
#include <vector>
#include <functional>

#include "../common/protocol.h"
#include "../common/xdg_paths.h"

namespace cec_control {

class SocketClient {
public:
    /**
     * Create socket client with default socket path from XDG paths
     */
    SocketClient() : SocketClient(XDGPaths::getDefaultSocketPath()) {}
    
    /**
     * Create socket client with specified socket path
     * @param socketPath Path to the socket file
     */
    explicit SocketClient(const std::string& socketPath);
    
    ~SocketClient();
    
    // Connect to daemon
    bool connect();
    
    // Disconnect from daemon
    void disconnect();
    
    // Send command and wait for response
    Message sendCommand(const Message& command);
    
    // Check if client is connected
    bool isConnected() const;

private:
    std::string m_socketPath;
    int m_socketFd;
    bool m_connected;
    
    // Receive response message from server
    Message receiveResponse();
};

} // namespace cec_control