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
    
    // Create thread pool - use a reasonable number of threads based on CPU cores
    // but limit to a maximum of 8 threads to avoid resource exhaustion
    unsigned int threadCount = std::min(std::thread::hardware_concurrency(), 8U);
    if (threadCount == 0) threadCount = 4; // Default if hardware_concurrency is not available
    
    m_threadPool = std::make_unique<ThreadPool>(threadCount);
    m_threadPool->start();
    LOG_INFO("Created thread pool with ", threadCount, " worker threads for client connections");
    
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
        for (int clientFd : m_activeClients) {
            // Close client socket
            if (clientFd >= 0) {
                shutdown(clientFd, SHUT_RDWR);
                close(clientFd);
            }
        }
        m_activeClients.clear();
    }
    
    // Shut down the thread pool
    if (m_threadPool) {
        LOG_INFO("Shutting down thread pool");
        m_threadPool->shutdown();
        m_threadPool.reset();
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
                    if (m_activeClients.size() >= 20) {  // Increased limit since we're using a thread pool
                        LOG_WARNING("Too many client connections, rejecting new client");
                        close(clientFd);
                        continue;
                    }
                    
                    // Add to active clients
                    m_activeClients.insert(clientFd);
                }
                
                try {
                    // Submit client handling task to thread pool
                    m_threadPool->submit([this, clientFd]() {
                        this->handleClient(clientFd);
                    });
                }
                catch (const std::exception& e) {
                    LOG_ERROR("Failed to submit client task to thread pool: ", e.what());
                    closeClient(clientFd);
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

void SocketServer::closeClient(int clientFd) {
    // Close the client socket and remove from active clients
    if (clientFd >= 0) {
        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
        
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_activeClients.erase(clientFd);
    }
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
    
    // Close client connection and clean up
    closeClient(clientFd);
}

} // namespace cec_control
