#include "socket_client.h"
#include "../common/logger.h"
#include "../common/system_paths.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <poll.h>
#include <filesystem>

namespace cec_control {

SocketClient::SocketClient(const std::string& socketPath)
    : m_socketFd(-1),
      m_connected(false) {
    
    if (socketPath.empty()) {
        m_socketPath = SystemPaths::getSocketPath();
    } else {
        m_socketPath = socketPath;
    }
}

SocketClient::~SocketClient() {
    disconnect();
}

bool SocketClient::connect() {
    // Create socket
    m_socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_socketFd < 0) {
        LOG_ERROR("Failed to create socket: ", strerror(errno));
        return false;
    }
    
    // First try the provided socket path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    LOG_DEBUG("Attempting to connect to socket at: ", m_socketPath);
    
    // Try primary socket path
    bool connected = false;
    if (::connect(m_socketFd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        connected = true;
        LOG_DEBUG("Connected to socket successfully at: ", m_socketPath);
    } else {
        // First connection attempt failed
        int originalErrno = errno;
        LOG_DEBUG("Failed to connect to primary socket: ", strerror(originalErrno));
        
        // If socket doesn't exist, try system location as fallback
        if (errno == ENOENT && 
            m_socketPath != "/run/cec-control/socket" && 
            !getenv("CEC_CONTROL_SOCKET")) {
            
            // Close original socket
            close(m_socketFd);
            
            // Try again with system socket
            std::string systemSocket = "/run/cec-control/socket";
            LOG_INFO("Trying system socket at ", systemSocket);
            
            // Create new socket
            m_socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_socketFd < 0) {
                LOG_ERROR("Failed to create socket for fallback: ", strerror(errno));
                return false;
            }
            
            // Connect to system socket
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, systemSocket.c_str(), sizeof(addr.sun_path) - 1);
            
            if (::connect(m_socketFd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                // Update the socket path we're using
                m_socketPath = systemSocket;
                connected = true;
                LOG_INFO("Connected to system socket successfully");
            }
        }
        
        // Handle connection failure with good error messages
        if (!connected) {
            if (errno == EACCES || errno == EPERM || originalErrno == EACCES || originalErrno == EPERM) {
                LOG_ERROR("Permission denied connecting to socket: ", m_socketPath);
                LOG_ERROR("The daemon may be running as a different user/systemd service");
                
                // Suggest using environment variable
                LOG_INFO("Try setting CEC_CONTROL_SOCKET=/run/cec-control/socket if running as system service");
            } else {
                LOG_ERROR("Failed to connect to server: ", strerror(errno));
                LOG_INFO("Socket path: ", m_socketPath);
                
                // Check if daemon is running
                LOG_ERROR("Socket file does not exist or daemon is not running");
            }
            
            close(m_socketFd);
            m_socketFd = -1;
            return false;
        }
    }
    
    m_connected = true;
    return true;
}

void SocketClient::disconnect() {
    if (m_socketFd >= 0) {
        close(m_socketFd);
        m_socketFd = -1;
    }
    m_connected = false;
}

Message SocketClient::sendCommand(const Message& command) {
    if (!m_connected) {
        LOG_ERROR("Not connected to server");
        return Message(MessageType::RESP_ERROR);
    }
    
    // Serialize and send command
    std::vector<uint8_t> data = Protocol::packMessage(command);
    
    ssize_t bytesSent = send(m_socketFd, data.data(), data.size(), 0);
    if (bytesSent < 0 || static_cast<size_t>(bytesSent) != data.size()) {
        LOG_ERROR("Failed to send command: ", strerror(errno));
        return Message(MessageType::RESP_ERROR);
    }
    
    // Wait for response with timeout
    return receiveResponse();
}

Message SocketClient::receiveResponse() {
    std::vector<uint8_t> buffer(1024);
    std::vector<uint8_t> receivedData;
    
    // Set timeout for response
    struct pollfd pfd;
    pfd.fd = m_socketFd;
    pfd.events = POLLIN;
    
    // Wait for up to 10 seconds for a response
    int pollResult = poll(&pfd, 1, 10000);
    
    if (pollResult < 0) {
        LOG_ERROR("Poll error: ", strerror(errno));
        return Message(MessageType::RESP_ERROR);
    } else if (pollResult == 0) {
        LOG_ERROR("Timeout waiting for response");
        return Message(MessageType::RESP_ERROR);
    }
    
    // Read response
    ssize_t bytesRead = recv(m_socketFd, buffer.data(), buffer.size(), 0);
    
    if (bytesRead <= 0) {
        if (bytesRead == 0) {
            LOG_ERROR("Server closed connection");
        } else {
            LOG_ERROR("Failed to receive response: ", strerror(errno));
        }
        return Message(MessageType::RESP_ERROR);
    }
    
    // Process the received data
    receivedData.insert(receivedData.end(), buffer.begin(), buffer.begin() + bytesRead);
    
    if (Protocol::validateMessage(receivedData)) {
        return Protocol::unpackMessage(receivedData);
    } else {
        LOG_ERROR("Invalid response received");
        return Message(MessageType::RESP_ERROR);
    }
}

bool SocketClient::isConnected() const {
    return m_connected;
}

} // namespace cec_control