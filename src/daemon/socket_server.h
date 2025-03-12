#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <mutex>
#include <functional>

#include "../common/protocol.h"

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
    std::mutex m_clientsMutex;
    std::unordered_map<int, std::thread> m_clientThreads;
    
    // Main server loop
    void serverLoop();
    
    // Handle individual client connections
    void handleClient(int clientFd);
    
    // Create and bind socket
    bool setupSocket();
    
    // Clean up socket resources
    void cleanupSocket();
    
    // Clean up all client threads
    void cleanupClientThreads();
    
    // Clean up only completed client threads
    void cleanupCompletedClientThreads();
};

} // namespace cec_control