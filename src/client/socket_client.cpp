#include "socket_client.h"
#include "../common/logger.h"
#include "../common/system_paths.h"
#include "../common/event_poller.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <chrono>
#include <algorithm>

namespace cec_control {

SocketClient::SocketClient(const std::string& socketPath)
    : m_socketFd(-1),
      m_connected(false) {
    
    m_socketPath = socketPath.empty() ? SystemPaths::getSocketPath() : socketPath;
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

    // Set up the address structure for the primary socket path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

    LOG_DEBUG("Attempting to connect to socket at: ", m_socketPath);

    // Try primary socket path
    if (::connect(m_socketFd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        LOG_DEBUG("Connected to socket successfully at: ", m_socketPath);
        m_connected = true;
        return true;
    }

    // Primary connection attempt failed
    int originalErrno = errno;
    LOG_DEBUG("Failed to connect to primary socket: ", m_socketPath, " error: ", strerror(originalErrno));

    std::string systemSocket = SystemPaths::getSocketPath(false);
    if ((originalErrno == ENOENT || originalErrno == EACCES || originalErrno == EPERM) &&
        m_socketPath != systemSocket && 
        !getenv("CEC_CONTROL_SOCKET")) {

        // Close original socket
        close(m_socketFd);

        // Try again with system socket
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
            m_connected = true;
            LOG_INFO("Connected to system socket successfully at: ", m_socketPath);
            return true;
        }

        LOG_DEBUG("Failed to connect to system socket: ", systemSocket, " error: ", strerror(errno));
    }

    // Handle connection failure with good error messages
    if (originalErrno == EACCES || originalErrno == EPERM || errno == EACCES || errno == EPERM) {
        LOG_ERROR("Permission denied connecting to socket: ", m_socketPath);
    } else if (originalErrno == ENOENT || errno == ENOENT) {
        LOG_ERROR("Socket file does not exist or daemon is not running at: ", m_socketPath);
    } else {
        LOG_ERROR("Failed to connect to server at: ", m_socketPath, " error: ", strerror(errno));
    }

    close(m_socketFd);
    m_socketFd = -1;
    return false;
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
        LOG_ERROR("Failed to send command to server: ", strerror(errno));
        return Message(MessageType::RESP_ERROR);
    }
    
    // Wait for response
    return receiveResponse();
}

Message SocketClient::receiveResponse() {
    const long long totalTimeoutMs = 10000;
    auto startTime = std::chrono::steady_clock::now();

    EventPoller poller;
    if (!poller.add(m_socketFd, static_cast<uint32_t>(EventPoller::Event::READ))) {
        LOG_ERROR("Failed to add socket to event poller");
        return Message(MessageType::RESP_ERROR);
    }

    auto timeRemaining = [&]() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
        return std::max(0LL, totalTimeoutMs - elapsed);
    };

    auto waitForRead = [&](long long timeoutMs) -> bool {
        const auto events = poller.wait(timeoutMs);
        if (events.empty()) {
            LOG_ERROR("Timeout waiting for response from server");
            return false;
        }
        for (const auto& event : events) {
            if (event.fd == m_socketFd) {
                if (event.events & EventPoller::ERROR_EVENTS) {
                    LOG_ERROR("Socket error during response receive");
                    return false;
                }
                if (event.events & static_cast<uint32_t>(EventPoller::Event::READ)) {
                    return true;
                }
            }
        }
        LOG_ERROR("No read event received for socket");
        return false;
    };

    std::vector<uint8_t> buffer;
    buffer.reserve(1024); // Pre-allocate some space

    // 1. Read header
    const size_t headerSize = 5;
    while (buffer.size() < headerSize) {
        if (!waitForRead(timeRemaining())) return Message(MessageType::RESP_ERROR);
        
        size_t oldSize = buffer.size();
        buffer.resize(headerSize);
        ssize_t bytesRead = recv(m_socketFd, buffer.data() + oldSize, headerSize - oldSize, 0);

        if (bytesRead <= 0) {
            LOG_ERROR(bytesRead == 0 ? "Server closed connection" : "Failed to receive response header: " + std::string(strerror(errno)));
            return Message(MessageType::RESP_ERROR);
        }
        buffer.resize(oldSize + bytesRead);
    }

    // 2. Validate header and get full message size
    if (buffer[0] != 'C' || buffer[1] != 'E' || buffer[2] != 'C') {
        LOG_ERROR("Invalid magic bytes in response");
        return Message(MessageType::RESP_ERROR);
    }
    uint16_t payloadSize = static_cast<uint16_t>(buffer[3]) | (static_cast<uint16_t>(buffer[4]) << 8);
    const size_t totalMessageSize = headerSize + payloadSize + 2; // header + payload + checksum

    // 3. Read rest of the message
    buffer.reserve(totalMessageSize);
    while (buffer.size() < totalMessageSize) {
        if (!waitForRead(timeRemaining())) return Message(MessageType::RESP_ERROR);

        size_t oldSize = buffer.size();
        buffer.resize(totalMessageSize);
        ssize_t bytesRead = recv(m_socketFd, buffer.data() + oldSize, totalMessageSize - oldSize, 0);

        if (bytesRead <= 0) {
            LOG_ERROR(bytesRead == 0 ? "Server closed connection" : "Failed to receive response body: " + std::string(strerror(errno)));
            return Message(MessageType::RESP_ERROR);
        }
        buffer.resize(oldSize + bytesRead);
    }

    // 4. Validate and unpack
    if (Protocol::validateMessage(buffer)) {
        return Protocol::unpackMessage(buffer);
    } else {
        LOG_ERROR("Invalid response received from server");
        return Message(MessageType::RESP_ERROR);
    }
}

bool SocketClient::isConnected() const {
    return m_connected;
}

} // namespace cec_control