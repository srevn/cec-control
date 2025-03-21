#include "socket_server.h"
#include "../common/logger.h"
#include "../common/buffer_manager.h"
#include "../common/system_paths.h"
#include "../common/protocol.h"
#include "../common/event_poller.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <cstring>
#include <cerrno>
#include <future>

namespace cec_control {

// Constants for configuration
constexpr int DEFAULT_THREAD_COUNT = 4;
constexpr int MAX_THREADS = 8;
constexpr int MAX_CLIENT_CONNECTIONS = 10;
constexpr int SERVER_POLL_TIMEOUT_MS = 100;
constexpr int CLIENT_POLL_TIMEOUT_MS = 100;
constexpr int SEND_POLL_TIMEOUT_MS = 500;
constexpr int THREAD_JOIN_TIMEOUT_SEC = 3;
constexpr int MAX_SERVER_CONSECUTIVE_ERRORS = 5;
constexpr int MAX_CLIENT_CONSECUTIVE_ERRORS = 3;
constexpr int CLIENT_RECV_TIMEOUT_SEC = 2;
constexpr mode_t SOCKET_FILE_PERMISSIONS = 0660;
constexpr int CLIENT_BUFFER_SIZE = 4096;
constexpr int DATA_BUFFER_SIZE = 8192;

SocketServer::SocketServer(const std::string& socketPath, std::shared_ptr<ThreadPool> threadPool)
    : m_socketPath(socketPath),
      m_socketFd(-1),
      m_running(false),
      m_threadPool(threadPool) {
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
    
    // Create thread pool if one wasn't provided
    if (!m_threadPool) {
        unsigned int threadCount = std::min(std::thread::hardware_concurrency(), static_cast<unsigned int>(MAX_THREADS));
        if (threadCount == 0) threadCount = DEFAULT_THREAD_COUNT;
        
        m_threadPool = std::make_shared<ThreadPool>(threadCount);
        m_threadPool->start();
        LOG_INFO("Created thread pool with ", threadCount, " worker threads for client connections");
    } else {
        LOG_INFO("Using shared thread pool for client connections");
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
    
    LOG_INFO("Stopping socket server");
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
            
            // Wait for thread to join with timeout
            if (joinFuture.wait_for(std::chrono::seconds(THREAD_JOIN_TIMEOUT_SEC)) == std::future_status::timeout) {
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
            if (clientFd >= 0) {
                shutdown(clientFd, SHUT_RDWR);
                close(clientFd);
            }
        }
        m_activeClients.clear();
    }
    
    // Shut down the thread pool
    if (m_threadPool && m_threadPool.use_count() <= 1) {
        LOG_INFO("Shutting down thread pool");
        m_threadPool->shutdown();
        m_threadPool.reset();
    }
    
    // Remove socket file
    cleanupSocket();
    
    LOG_INFO("Socket server stopped completely");
}

void SocketServer::setCommandHandler(ClientHandler handler) {
    m_cmdHandler = std::move(handler);
}

bool SocketServer::isRunning() const {
    return m_running;
}

bool SocketServer::setupSocket() {
    // Create socket
    m_socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_socketFd < 0) {
        LOG_ERROR("Failed to create server socket: ", strerror(errno));
        return false;
    }
    
    // Set socket to non-blocking mode
    if (!setNonBlocking(m_socketFd)) {
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }
    
    // Remove existing socket file if it exists
    cleanupSocket();
    
    // Ensure parent directory exists
    std::string parentDir = m_socketPath.substr(0, m_socketPath.find_last_of('/'));
    if (!SystemPaths::createDirectories(parentDir)) {
        LOG_ERROR("Failed to create parent directory for socket: ", parentDir);
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }
    
    // Verify directory permissions
    if (access(parentDir.c_str(), W_OK) != 0) {
        LOG_ERROR("Insufficient permissions for socket directory: ", parentDir);
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }

    // Bind socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);
    
    if (bind(m_socketFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind socket to path: ", m_socketPath, " error: ", strerror(errno));
        close(m_socketFd);
        m_socketFd = -1;
        return false;
    }

    // Set socket file permissions
    if (chmod(m_socketPath.c_str(), SOCKET_FILE_PERMISSIONS) != 0) {
        LOG_WARNING("Failed to set socket permissions: ", strerror(errno));
    }
    
    // Listen for connections
    if (listen(m_socketFd, 5) < 0) {
        LOG_ERROR("Failed to listen on socket: ", m_socketPath, " error: ", strerror(errno));
        close(m_socketFd);
        m_socketFd = -1;
        cleanupSocket();
        return false;
    }

    return true;
}

void SocketServer::cleanupSocket() {
    if (access(m_socketPath.c_str(), F_OK) != -1) {
        if (unlink(m_socketPath.c_str()) != 0) {
            LOG_WARNING("Failed to remove socket file: ", m_socketPath, " error: ", strerror(errno));
        }
    }
}

void SocketServer::serverLoop() {
    try {
        EventPoller poller;
        
        // Add server socket to the poller
        poller.add(m_socketFd, static_cast<uint32_t>(EventPoller::Event::READ));
        
        int consecutiveErrors = 0;
        
        while (m_running) {
            // Wait for events with a short timeout
            auto events = poller.wait(SERVER_POLL_TIMEOUT_MS);
            
            if (events.empty()) {
                // Timeout - reset error counter and continue
                consecutiveErrors = 0;
                continue;
            }
            
            for (const auto& event : events) {
                if (event.fd == m_socketFd) {
                    // Check for socket errors
                    if (event.events & EventPoller::ERROR_EVENTS) {
                        LOG_ERROR("Server socket error detected, trying to recover");
                        
                        // Try to recreate the socket
                        close(m_socketFd);
                        poller.remove(m_socketFd);
                        
                        if (!setupSocket()) {
                            LOG_ERROR("Failed to restore server socket, exiting server loop");
                            break;
                        }
                        
                        poller.add(m_socketFd, static_cast<uint32_t>(EventPoller::Event::READ));
                        continue;
                    }
                    
                    // Connection waiting - accept it
                    if (event.events & static_cast<uint32_t>(EventPoller::Event::READ)) {
                        // In edge-triggered mode, we need to accept all connections
                        while (m_running) {
                            struct sockaddr_un clientAddr;
                            socklen_t clientLen = sizeof(clientAddr);
                            
                            int clientFd = accept(m_socketFd, (struct sockaddr*)&clientAddr, &clientLen);
                            if (clientFd < 0) {
                                // If we've accepted all pending connections, break
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    break;
                                }
                                
                                // If server is stopping, this error is expected
                                if (m_running && errno != EINTR) {
                                    LOG_ERROR("Failed to accept connection: ", strerror(errno));
                                    consecutiveErrors++;
                                }
                                
                                // If socket was closed, we're probably shutting down
                                if (errno == EBADF || errno == EINVAL) {
                                    LOG_INFO("Server socket closed, exiting server loop");
                                    m_running = false;
                                }
                                
                                break;
                            }
                            
                            // Successfully accepted a connection, reset error counter
                            consecutiveErrors = 0;
                            
                            // Set client socket to non-blocking
                            if (!setNonBlocking(clientFd)) {
                                close(clientFd);
                                continue;
                            }
                            
                            LOG_INFO("Client connected on fd: ", clientFd);
                            
                            // Limit the number of concurrent client connections
                            {
                                std::lock_guard<std::mutex> lock(m_clientsMutex);
                                if (m_activeClients.size() >= MAX_CLIENT_CONNECTIONS) {
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
                                LOG_ERROR("Failed to submit client task: ", e.what());
                                closeClient(clientFd);
                            }
                        }
                    }
                }
            }
            
            // Check if too many consecutive errors
            if (consecutiveErrors >= MAX_SERVER_CONSECUTIVE_ERRORS) {
                LOG_ERROR("Too many consecutive errors, restarting server loop");
                consecutiveErrors = 0;
                
                // Try to recreate the socket
                close(m_socketFd);
                poller.remove(m_socketFd);
                
                if (!setupSocket()) {
                    LOG_ERROR("Failed to restore server socket, exiting server loop");
                    break;
                }
                
                poller.add(m_socketFd, static_cast<uint32_t>(EventPoller::Event::READ));
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
    if (clientFd >= 0) {
        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
        
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_activeClients.erase(clientFd);
    }
}

bool SocketServer::sendDataToClient(int clientFd, const Message& cmd) {
    // Process command
    Message response = m_cmdHandler ? m_cmdHandler(cmd) : Message(MessageType::RESP_ERROR);
    
    // Pack response
    std::vector<uint8_t> responseData = Protocol::packMessage(response);
    size_t totalSent = 0;
    
    // Setup event poller for writing
    EventPoller poller;
    poller.add(clientFd, static_cast<uint32_t>(EventPoller::Event::WRITE));
    
    // Send the response with timeout
    while (totalSent < responseData.size()) {
        auto events = poller.wait(SEND_POLL_TIMEOUT_MS);
        
        if (events.empty()) {
            LOG_ERROR("Timeout waiting to send response");
            return false;
        }
        
        bool canWrite = false;
        for (const auto& event : events) {
            if (event.fd == clientFd && (event.events & static_cast<uint32_t>(EventPoller::Event::WRITE))) {
                canWrite = true;
                break;
            }
        }
        
        if (!canWrite) {
            LOG_ERROR("Error waiting to send response");
            return false;
        }
        
        ssize_t sent = send(clientFd, 
                           responseData.data() + totalSent, 
                           responseData.size() - totalSent, 
                           MSG_NOSIGNAL);
        
        if (sent > 0) {
            totalSent += sent;
        } else if (sent < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Error sending response: ", strerror(errno));
                return false;
            }
        }
    }
    
    return true;
}

void SocketServer::handleClient(int clientFd) {
    try {
        // Set socket options for reliability
        int keepAlive = 1;
        setsockopt(clientFd, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));
        
        // Set receive timeout
        struct timeval tv;
        tv.tv_sec = CLIENT_RECV_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        // Set up event poller for more responsive I/O
        EventPoller poller;
        poller.add(clientFd, static_cast<uint32_t>(EventPoller::Event::READ));
        
        // Get buffers from pool
        auto& bufferPool = BufferPoolManager::getInstance().getPool(CLIENT_BUFFER_SIZE);
        auto recvBuffer = bufferPool.acquireBuffer();
        
        auto& dataBufferPool = BufferPoolManager::getInstance().getPool(DATA_BUFFER_SIZE);
        auto receivedData = dataBufferPool.acquireBuffer();
        receivedData->reserve(DATA_BUFFER_SIZE);
        
        bool connectionActive = true;
        int consecutiveErrors = 0;
        
        while (connectionActive && m_running) {
            try {
                // Wait for data with a short timeout
                auto events = poller.wait(CLIENT_POLL_TIMEOUT_MS);
                
                if (events.empty()) {
                    // Timeout - reset error counter
                    consecutiveErrors = 0;
                    continue;
                }
                
                for (const auto& event : events) {
                    if (event.fd == clientFd) {
                        // Check for socket errors
                        if (event.events & EventPoller::ERROR_EVENTS) {
                            if (event.events & static_cast<uint32_t>(EventPoller::Event::ERROR)) 
                                LOG_WARNING("Error event on client fd ", clientFd);
                            if (event.events & static_cast<uint32_t>(EventPoller::Event::HANGUP)) 
                                LOG_DEBUG("Client disconnected normally on fd ", clientFd);
                            if (event.events & static_cast<uint32_t>(EventPoller::Event::INVALID)) 
                                LOG_WARNING("Invalid socket fd ", clientFd);
                            connectionActive = false;
                            continue;
                        }
                        
                        consecutiveErrors = 0;
                        
                        // Data is available to read
                        if (event.events & static_cast<uint32_t>(EventPoller::Event::READ)) {
                            // In edge-triggered mode, we need to read all data
                            while (connectionActive) {
                                recvBuffer->resize(CLIENT_BUFFER_SIZE);
                                
                                ssize_t bytesRead = recv(clientFd, recvBuffer->data(), recvBuffer->size(), 0);
                                
                                if (bytesRead > 0) {
                                    // Append data to received buffer
                                    receivedData->insert(receivedData->end(), recvBuffer->begin(), recvBuffer->begin() + bytesRead);
                                    
                                    // Process complete messages
                                    while (!receivedData->empty() && Protocol::validateMessage(*receivedData)) {
                                        try {
                                            // Extract and process message
                                            Message cmd = Protocol::unpackMessage(*receivedData);
                                            
                                            // Process and send response
                                            if (!sendDataToClient(clientFd, cmd)) {
                                                connectionActive = false;
                                                break;
                                            }
                                            
                                            // Remove processed message
                                            if (receivedData->size() >= 5) {
                                                uint16_t size = static_cast<uint16_t>((*receivedData)[3]) | 
                                                              (static_cast<uint16_t>((*receivedData)[4]) << 8);
                                                size_t msgSize = size + 7; // header (5) + payload (size) + checksum (2)
                                                
                                                if (receivedData->size() < msgSize) {
                                                    // Incomplete message, clear buffer
                                                    receivedData->clear();
                                                    break;
                                                }
                                                
                                                receivedData->erase(receivedData->begin(), receivedData->begin() + msgSize);
                                            } else {
                                                // Not enough data for a valid header
                                                break;
                                            }
                                        }
                                        catch (const std::exception& e) {
                                            LOG_ERROR("Exception processing message: ", e.what());
                                            receivedData->clear();
                                            break;
                                        }
                                    }
                                }
                                else if (bytesRead == 0) {
                                    // Client disconnected
                                    LOG_DEBUG("Client closed connection on fd ", clientFd);
                                    connectionActive = false;
                                    break;
                                }
                                else {
                                    // Error reading or would block
                                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                        // No more data available for now
                                        break;
                                    } else {
                                        LOG_ERROR("Error reading from client: ", strerror(errno));
                                        connectionActive = false;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("Exception in client handler: ", e.what());
                consecutiveErrors++;
                if (consecutiveErrors >= MAX_CLIENT_CONSECUTIVE_ERRORS) {
                    connectionActive = false;
                }
            }
        }
        
        // Return buffers to pools
        bufferPool.releaseBuffer(std::move(recvBuffer));
        dataBufferPool.releaseBuffer(std::move(receivedData));
    }
    catch (const std::exception& e) {
        LOG_ERROR("Unhandled exception in client handler: ", e.what());
    }
    catch (...) {
        LOG_ERROR("Unknown exception in client handler");
    }
    
    LOG_INFO("Client disconnected from fd ", clientFd);
    closeClient(clientFd);
}

bool SocketServer::setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        LOG_ERROR("Failed to get socket flags: ", strerror(errno));
        return false;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_ERROR("Failed to set socket non-blocking: ", strerror(errno));
        return false;
    }
    
    return true;
}

} // namespace cec_control