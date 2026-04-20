#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../common/messages.h"
#include "../common/system_paths.h"
#include "../common/unix_socket.h"
#include "socket_connection.h"
#include "thread_pool.h"

namespace cec_control {

/**
 * Unix-socket listener whose accept queue is drained from the unified
 * event loop.
 *
 * start() provisions the listening socket and returns; there is no
 * dedicated accept thread. Call listenerFd() to register with the
 * EventLoop; its handler calls onReadable(), which drains pending
 * accepts and hands each one to the supplied ThreadPool via
 * ConnectionManager.
 *
 * stop() closes the listener (so no new connections are accepted) and
 * drains all in-flight connection handlers. The thread pool is borrowed,
 * never owned — the caller outlives this object.
 */
class SocketServer {
public:
    using CommandHandler = std::function<Message(const Message&)>;

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;
    SocketServer(SocketServer&&) = delete;
    SocketServer& operator=(SocketServer&&) = delete;

    /**
     * @param threadPool  Pool used to run connection handlers. Must be
     *                    non-null and outlive this object.
     */
    explicit SocketServer(std::shared_ptr<ThreadPool> threadPool)
        : SocketServer(SystemPaths::getSocketPath(), std::move(threadPool)) {}

    SocketServer(std::string socketPath, std::shared_ptr<ThreadPool> threadPool);

    ~SocketServer();

    /**
     * Open the listener. Returns false if binding/listening fails or if
     * another process is already accepting on the socket path.
     */
    [[nodiscard]] bool start();

    /** Close the listener and drain all live connections. Idempotent. */
    void stop();

    /** Install/replace the per-request handler. */
    void setCommandHandler(CommandHandler handler);

    /** The listener fd for EventLoop registration. -1 if not started. */
    int listenerFd() const noexcept { return m_listener.get(); }

    /**
     * Handler invoked by the event loop when the listener is readable.
     * Drains the accept queue; one invocation may produce zero or more
     * new connections.
     */
    void onReadable();

    bool isRunning() const noexcept { return m_listener.valid(); }

private:
    std::string m_socketPath;
    std::shared_ptr<ThreadPool> m_threadPool;
    CommandHandler m_handler;
    UnixSocket m_listener;
    ConnectionManager m_connections;
};

} // namespace cec_control
