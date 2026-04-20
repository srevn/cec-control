#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../common/messages.h"
#include "../common/system_paths.h"
#include "../common/unix_socket.h"
#include "socket_connection.h"
#include "thread_pool.h"

namespace cec_control {

/**
 * Accepts client connections on a Unix SEQPACKET socket and dispatches each
 * one to a worker in the provided ThreadPool. Connection lifetime and FD
 * ownership live entirely in socket_connection.{h,cpp}; this class only runs
 * the accept loop and coordinates startup/shutdown.
 */
class SocketServer {
public:
    using CommandHandler = std::function<Message(const Message&)>;

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;
    SocketServer(SocketServer&&) = delete;
    SocketServer& operator=(SocketServer&&) = delete;

    explicit SocketServer(std::shared_ptr<ThreadPool> threadPool = nullptr)
        : SocketServer(SystemPaths::getSocketPath(), std::move(threadPool)) {}

    SocketServer(std::string socketPath,
                 std::shared_ptr<ThreadPool> threadPool = nullptr);

    ~SocketServer();

    bool start();
    void stop();

    void setCommandHandler(CommandHandler handler);

    bool isRunning() const noexcept { return m_running.load(); }

private:
    void acceptLoop();
    bool wakeupAcceptLoop() noexcept;

    std::string m_socketPath;
    std::shared_ptr<ThreadPool> m_threadPool;
    bool m_ownsThreadPool = false;

    CommandHandler m_handler;

    UnixSocket m_listener;
    int m_wakeFd = -1;                  // eventfd used to unblock acceptLoop
    std::atomic<bool> m_running{false};
    std::thread m_acceptThread;

    ConnectionManager m_connections;
};

} // namespace cec_control
