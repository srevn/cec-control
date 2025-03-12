#include "socket_server.h"
#include "../common/logger.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <cstring>
#include <cerrno>
#include <future>

namespace cec_control {

SocketServer::SocketServer(const std::string& socketPath)
    : m_socketPath(socketPath),
      m_socketFd(-1),
      m_running(false),
      m_cmdHandler(nullptr) {
}

SocketServer::~SocketServer() {
    stop();
}

bool SocketServer::start() {
    if (m_running) {
        LOG_WARNING("Socket server already running");
        return true;
    }
    
    if (!setupSocket()) {
        return false;
    }
    
    m_running = true;
    m_serverThread = std::thread(&SocketServer::serverLoop, this);
    
    LOG_INFO("Socket server started on ", m_socketPath);
    return true;
}

void SocketServer::stop() {
    if (!m_running) {
        LOG_INFO("Socket server already stopped");
        return;
    }
    
    LOG_INFO("Stopping socket server - setting running flag to false");
    
    m_running = false;
    
    // Close socket to interrupt accept() in server loop
    if (m_socketFd >= 0) {
        LOG_INFO("Closing server socket");
        close(m_socketFd);
        m_socketFd = -1;
    }
    
    // Wait for server thread to finish with timeout
    if (m_serverThread.joinable()) {
        LOG_INFO("Waiting for server thread to exit");
        try {
            // Use async with future to implement timeout for thread join
            auto joinFuture = std::async(std::launch::async, [this]() {
                if (m_serverThread.joinable()) {
                    m_serverThread.join();
                    return true;
                }
                return false;
            });
            
            // Wait for thread to join with 3 second timeout
            if (joinFuture.wait_for(std::chrono::seconds(3)) == std::future_status::timeout) {
                LOG_WARNING("Server thread did not exit cleanly within timeout, detaching");
                if (m_serverThread.joinable()) {
                    m_serverThread.detach();
                }
            } else {
                LOG_INFO("Server thread joined successfully");
            }
        }
        catch (const std::exception& e) {
            LOG_ERROR("Exception joining server thread: ", e.what());
            if (m_serverThread.joinable()) {
                m_serverThread.detach();
            }
        }
    }
    
    // Close all client connections
    {
        LOG_INFO("Closing all client connections");
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clientThreads) {
            // Close client socket
            if (pair.first >= 0) {
                close(pair.first);
            }
            
            // Detach threads that can't be joined safely
            if (pair.second.joinable() && 
                std::this_thread::get_id() == pair.second.get_id()) {
                pair.second.detach();
            }
        }
    }
    
    // Clean up client threads with timeout
    try {
        LOG_INFO("Cleaning up client threads");
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto it = m_clientThreads.begin(); it != m_clientThreads.end(); ) {
            if (it->second.joinable()) {
                auto joinFuture = std::async(std::launch::async, [&thread = it->second]() {
                    if (thread.joinable()) {
                        thread.join();
                        return true;
                    }
                    return false;
                });
                
                // Wait for thread to join with 2 second timeout
                if (joinFuture.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
                    LOG_WARNING("Client thread did not exit cleanly within timeout, detaching");
                    if (it->second.joinable()) {
                        it->second.detach();
                    }
                }
            }
            it = m_clientThreads.erase(it);
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception cleaning up client threads: ", e.what());
        
        // Last resort: detach any remaining threads
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        for (auto& pair : m_clientThreads) {
            if (pair.second.joinable()) {
                pair.second.detach();
            }
        }
        m_clientThreads.clear();
    }
    
    // Remove socket file
    cleanupSocket();
    
    LOG_INFO("Socket server stopped completely");
}

void SocketServer::setCommandHandler(ClientHandler handler) {
    m_cmdHandler = handler;
}

bool SocketServer::isRunning() const {
    return m_running;
}

bool SocketServer::setupSocket() {
    // Create socket
    m_socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_socketFd < 0) {
        LOG_ERROR("Failed to create socket: ", strerror(errno));
        return false;
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(m_socketFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_WARNING("Failed to set socket options: ", strerror(errno));
    }
    
    // Remove existing socket file if it exists
    cleanupSocket();
    
    // Bind socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    if (bind(m_socketFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind socket: ", strerror(errno));
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }
    
    // Set permissions for socket file
    chmod(m_socketPath.c_str(), 0666);
    
    // Listen for connections
    if (listen(m_socketFd, 5) < 0) {
        LOG_ERROR("Failed to listen on socket: ", strerror(errno));
        close(m_socketFd);
        m_socketFd = -1;
        cleanupSocket();
        return false;
    }
    
    return true;
}

void SocketServer::cleanupSocket() {
    // Remove the socket file if it exists
    if (access(m_socketPath.c_str(), F_OK) != -1) {
        unlink(m_socketPath.c_str());
    }
}

void SocketServer::cleanupClientThreads() {
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    for (auto it = m_clientThreads.begin(); it != m_clientThreads.end(); ) {
        if (it->second.joinable()) {
            it->second.join();
        }
        it = m_clientThreads.erase(it);
    }
}

void SocketServer::cleanupCompletedClientThreads() {
    // Caller must hold m_clientsMutex lock
    // Note: In this implementation we're only cleaning up detached threads since
    // we can't check if a thread is completed without blocking.
    // Completed threads will be cleaned up during server shutdown.
    
    // We can remove thread entries that have been detached
    for (auto it = m_clientThreads.begin(); it != m_clientThreads.end(); ) {
        if (!it->second.joinable()) {
            // Thread is already detached or not valid
            it = m_clientThreads.erase(it);
        } else {
            ++it;
        }
    }
}

void SocketServer::serverLoop() {
    try {
        // Set up poll structure for the server socket
        struct pollfd pfd;
        pfd.fd = m_socketFd;
        pfd.events = POLLIN;
        
        int consecutiveErrors = 0;
        const int MAX_CONSECUTIVE_ERRORS = 5;
        
        // Set server socket to non-blocking mode
        int flags = fcntl(m_socketFd, F_GETFL, 0);
        fcntl(m_socketFd, F_SETFL, flags | O_NONBLOCK);
        
        while (m_running) {
            // Use poll to wait for connections
            int pollResult = poll(&pfd, 1, 100);  // 100ms timeout
            
            if (pollResult < 0) {
                // Error in poll
                if (errno != EINTR) {
                    LOG_ERROR("Server poll error: ", strerror(errno));
                    consecutiveErrors++;
                    
                    if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
                        LOG_ERROR("Too many consecutive poll errors, restarting server loop");
                        // Reset error counter and continue
                        consecutiveErrors = 0;
                    }
                }
                continue;
            } 
            else if (pollResult == 0) {
                // Timeout - no connection waiting
                consecutiveErrors = 0;  // Reset error counter on successful poll
                continue;
            }
            
            // Check for socket errors
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                LOG_ERROR("Server socket error detected, trying to recover");
                
                // Try to recreate the socket
                close(m_socketFd);
                if (!setupSocket()) {
                    LOG_ERROR("Failed to restore server socket, exiting server loop");
                    break;
                }
                
                // Update poll fd with new socket
                pfd.fd = m_socketFd;
                continue;
            }
            
            // Connection waiting - accept it
            if (pfd.revents & POLLIN) {
                struct sockaddr_un clientAddr;
                socklen_t clientLen = sizeof(clientAddr);
                
                int clientFd = accept(m_socketFd, (struct sockaddr*)&clientAddr, &clientLen);
                if (clientFd < 0) {
                    // If server is stopping, this error is expected
                    if (m_running && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                        LOG_ERROR("Failed to accept connection: ", strerror(errno));
                        consecutiveErrors++;
                    }
                    
                    // If socket was closed, we're probably shutting down
                    if (errno == EBADF || errno == EINVAL) {
                        LOG_INFO("Socket closed, server loop exiting");
                        break;
                    }
                    
                    continue;
                }
                
                // Successfully accepted a connection, reset error counter
                consecutiveErrors = 0;
                
                // Set client socket to non-blocking
                int clientFlags = fcntl(clientFd, F_GETFL, 0);
                fcntl(clientFd, F_SETFL, clientFlags | O_NONBLOCK);
                
                LOG_INFO("Client connected");
                
                // Limit the number of concurrent client connections
                {
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    if (m_clientThreads.size() >= 10) {  // Arbitrary limit to prevent resource exhaustion
                        LOG_WARNING("Too many client connections, rejecting new client");
                        close(clientFd);
                        continue;
                    }
                }
                
                try {
                    // Start client handler thread with appropriate priority
                    std::lock_guard<std::mutex> lock(m_clientsMutex);
                    m_clientThreads[clientFd] = std::thread(&SocketServer::handleClient, this, clientFd);
                    
                    // Clean up any completed client threads
                    cleanupCompletedClientThreads();
                }
                catch (const std::exception& e) {
                    LOG_ERROR("Failed to create client thread: ", e.what());
                    close(clientFd);
                }
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Unhandled exception in server loop: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception in server loop");
    }
    
    LOG_INFO("Server loop exiting");
}

void SocketServer::handleClient(int clientFd) {
    try {
        // Set socket options for reliability
        int keepAlive = 1;
        if (setsockopt(clientFd, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive)) < 0) {
            LOG_WARNING("Failed to set SO_KEEPALIVE: ", strerror(errno));
        }
        
        // Set TCP no delay to ensure commands are sent immediately
        int tcpNoDelay = 1;
        if (setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &tcpNoDelay, sizeof(tcpNoDelay)) < 0) {
            // Unix socket may not support TCP_NODELAY, so just log as debug
            LOG_DEBUG("Failed to set TCP_NODELAY: ", strerror(errno));
        }
        
        // Set receive timeout to ensure we don't block indefinitely
        struct timeval tv;
        tv.tv_sec = 2;  // 2 second timeout
        tv.tv_usec = 0;
        if (setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            LOG_WARNING("Failed to set SO_RCVTIMEO: ", strerror(errno));
        }
        
        // Set up poll structure for more responsive I/O
        struct pollfd pfd;
        pfd.fd = clientFd;
        pfd.events = POLLIN;
        
        std::vector<uint8_t> buffer(4096);  // Larger buffer for efficiency
        std::vector<uint8_t> receivedData;
        receivedData.reserve(8192);  // Reserve space to reduce reallocations
        bool connectionActive = true;
        
        int consecutiveErrors = 0;
        const int MAX_CONSECUTIVE_ERRORS = 3;
        
        while (connectionActive && m_running) {
            try {
                // Wait for data with a shorter timeout for better responsiveness
                int pollResult = poll(&pfd, 1, 50);
                
                if (pollResult < 0) {
                    // Error in poll
                    if (errno != EINTR) {
                        LOG_ERROR("Poll error: ", strerror(errno));
                        consecutiveErrors++;
                        if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
                            LOG_ERROR("Too many consecutive poll errors, closing connection");
                            connectionActive = false;
                        }
                    }
                    continue;
                }
                else if (pollResult == 0) {
                    // Timeout - no data available, reset error counter
                    consecutiveErrors = 0;
                    continue;
                }
                
                // Check for socket errors
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    if (pfd.revents & POLLERR) {
                        LOG_WARNING("Socket error detected");
                    } else if (pfd.revents & POLLHUP) {
                        LOG_DEBUG("Socket hangup detected (client disconnected normally)");
                    } else if (pfd.revents & POLLNVAL) {
                        LOG_WARNING("Socket invalid (possibly already closed)");
                    }
                    connectionActive = false;
                    continue;
                }
                
                // Reset error counter when we get valid poll events
                consecutiveErrors = 0;
                
                // Data is available to read
                if (pfd.revents & POLLIN) {
                    ssize_t bytesRead = recv(clientFd, buffer.data(), buffer.size(), 0);
                    
                    if (bytesRead > 0) {
                        // Append data to received buffer
                        receivedData.insert(receivedData.end(), buffer.begin(), buffer.begin() + bytesRead);
                        
                        // Process complete messages
                        while (!receivedData.empty() && Protocol::validateMessage(receivedData)) {
                            try {
                                // Extract and process message
                                Message cmd = Protocol::unpackMessage(receivedData);
                                
                                // Process command if handler is set
                                Message response;
                                if (m_cmdHandler) {
                                    response = m_cmdHandler(cmd);
                                } else {
                                    response = Message(MessageType::RESP_ERROR);
                                }
                                
                                // Send response and ensure it's fully sent
                                std::vector<uint8_t> responseData = Protocol::packMessage(response);
                                size_t totalSent = 0;
                                
                                // Setup polling for writing to ensure we don't block indefinitely
                                struct pollfd sendPfd;
                                sendPfd.fd = clientFd;
                                sendPfd.events = POLLOUT;
                                
                                while (totalSent < responseData.size()) {
                                    // Wait for socket to be ready for writing
                                    if (poll(&sendPfd, 1, 500) > 0 && (sendPfd.revents & POLLOUT)) {
                                        ssize_t sent = send(clientFd, 
                                                            responseData.data() + totalSent, 
                                                            responseData.size() - totalSent, 
                                                            MSG_NOSIGNAL);  // Prevent SIGPIPE signals
                                        
                                        if (sent > 0) {
                                            totalSent += sent;
                                        } else if (sent < 0) {
                                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                                LOG_ERROR("Error sending response: ", strerror(errno));
                                                connectionActive = false;
                                                break;
                                            }
                                        }
                                    } else {
                                        LOG_ERROR("Timeout or error while waiting to send response");
                                        connectionActive = false;
                                        break;
                                    }
                                }
                                
                                // If we couldn't send the full response, exit the message processing loop
                                if (!connectionActive) {
                                    break;
                                }
                                
                                // Remove processed message from buffer
                                if (receivedData.size() >= 5) { // Ensure we have enough bytes for the header
                                    uint16_t size = static_cast<uint16_t>(receivedData[3]) | (static_cast<uint16_t>(receivedData[4]) << 8);
                                    size_t msgSize = size + 7;  // 3 magic bytes + 2 size bytes + 2 checksum bytes
                                    
                                    if (receivedData.size() < msgSize) {
                                        // Something went wrong, clear buffer
                                        receivedData.clear();
                                        break;
                                    }
                                    
                                    receivedData.erase(receivedData.begin(), receivedData.begin() + msgSize);
                                } else {
                                    // Not enough data for a complete header
                                    break;
                                }
                            }
                            catch (const std::exception& e) {
                                LOG_ERROR("Exception processing message: ", e.what());
                                receivedData.clear();
                                break;
                            }
                        }
                    }
                    else if (bytesRead == 0) {
                        // Client disconnected gracefully
                        LOG_DEBUG("Client closed connection gracefully");
                        connectionActive = false;
                    }
                    else {
                        // Error reading
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // Non-blocking socket timeout, continue
                            LOG_DEBUG("Socket read timeout (non-blocking)");
                        } else {
                            LOG_ERROR("Error reading from client: ", strerror(errno));
                            connectionActive = false;
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("Exception in client handler loop: ", e.what());
                consecutiveErrors++;
                if (consecutiveErrors >= MAX_CONSECUTIVE_ERRORS) {
                    LOG_ERROR("Too many consecutive errors, closing connection");
                    connectionActive = false;
                }
            }
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Unhandled exception in client handler: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception in client handler");
    }
    
    LOG_INFO("Client disconnected");
    
    // Ensure socket has been properly shutdown before closing
    shutdown(clientFd, SHUT_RDWR);
    
    // Close client socket if still open
    if (clientFd >= 0) {
        close(clientFd);
    }
    
    // Mark this thread for cleanup but do not join itself
    try {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        // We can't join our own thread, just remove it from the map
        // The thread object will be cleaned up when the SocketServer is destroyed
        auto it = m_clientThreads.find(clientFd);
        if (it != m_clientThreads.end()) {
            // Make it safe to detach if thread's ID is the current thread ID
            if (std::this_thread::get_id() == it->second.get_id()) {
                it->second.detach();
            }
            // Otherwise let the other thread finish and join properly
            m_clientThreads.erase(it);
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Exception during client thread cleanup: ", e.what());
    }
}

} // namespace cec_control
