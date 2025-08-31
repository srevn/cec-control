#pragma once

#include <string>
#include <vector>
#include <functional>

#include "../common/protocol.h"
#include "../common/system_paths.h"

namespace cec_control {

class SocketClient {
public:
    /**
     * Create socket client with default system socket path
     */
    SocketClient() : SocketClient(SystemPaths::getSocketPath()) {}
    
    /**
     * Create socket client with specified socket path
     * @param socketPath Path to the socket file
     */
    explicit SocketClient(const std::string& socketPath);
    
    ~SocketClient();
    
    /**
     * Connect to daemon
     * @return True if connection was successful, false otherwise
     */
    bool connect();
    
    /**
     * Disconnect from daemon
     */
    void disconnect();
    
    /**
     * Send command to daemon and wait for response
     * @param command Command to send
     * @return Response from daemon
     */
    Message sendCommand(const Message& command);
    
    /**
     * Check if client is connected to daemon
     * @return True if connected, false otherwise
     */
    bool isConnected() const;

private:
    std::string m_socketPath;
    int m_socketFd;
    bool m_connected;
    
    // Receive response message from server
    Message receiveResponse();

    /**
     * Set a socket to non-blocking mode
     * @param fd Socket file descriptor
     * @return True if successful
     */
    bool setNonBlocking(int fd);
};

} // namespace cec_control