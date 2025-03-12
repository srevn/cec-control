#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>

#include "../common/protocol.h"
#include "thread_pool.h"

namespace cec_control {

class SocketServer {
public:
    using ClientHandler = std::function<Message(const Message&)>;
    
    explicit SocketServer(const std::string& socketPath = SOCKET_PATH);
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
    std::unique_ptr<ThreadPool> m_threadPool;
    
    // Active client connections
    std::mutex m_clientsMutex;
    std::unordered_set<int> m_activeClients;
    
    // Command coordination
    std::mutex m_commandMutex;
    
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
};

} // namespace cec_control