#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>

#include "../common/protocol.h"
#include "../common/system_paths.h"
#include "thread_pool.h"

namespace cec_control {

class SocketServer {
public:
    using ClientHandler = std::function<Message(const Message&)>;
    
    // Disable copying and moving
    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;
    SocketServer(SocketServer&&) = delete;
    SocketServer& operator=(SocketServer&&) = delete;
    
    /**
     * Create socket server with default system socket path
     * @param threadPool Optional external thread pool to use
     */
    SocketServer(std::shared_ptr<ThreadPool> threadPool = nullptr) 
        : SocketServer(SystemPaths::getSocketPath(), threadPool) {}
    
    /**
     * Create socket server with specified socket path
     * @param socketPath Path to the socket file
     * @param threadPool Optional external thread pool to use
     */
    explicit SocketServer(const std::string& socketPath, std::shared_ptr<ThreadPool> threadPool = nullptr);
    
    ~SocketServer();
    
    // Start server in a separate thread
    bool start();
    
    // Stop server gracefully
    void stop();
    
    // Set command handler callback
    void setCommandHandler(ClientHandler handler);
    
    // Check if server is running
    bool isRunning() const;

private:
    std::string m_socketPath;
    int m_socketFd;
    std::atomic<bool> m_running;
    std::thread m_serverThread;
    ClientHandler m_cmdHandler;
    
    // Thread pool for client connections
    std::shared_ptr<ThreadPool> m_threadPool;
    
    // Active client connections
    std::mutex m_clientsMutex;
    std::unordered_set<int> m_activeClients;
    
    // Main server loop
    void serverLoop();
    
    // Handle individual client connections
    void handleClient(int clientFd);
    
    // Create and bind socket
    bool setupSocket();
    
    // Clean up socket resources
    void cleanupSocket();
    
    // Close a client connection
    void closeClient(int clientFd);
    
    /**
     * Process a command message and send the response back to the client
     * @param clientFd File descriptor for the client connection
     * @param cmd The message/command to process
     * @return true if successful, false otherwise
     */
    bool processAndSendResponse(int clientFd, const Message& cmd);
};

} // namespace cec_control
