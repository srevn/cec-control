#include "socket_server.h"

#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <utility>

#include "../common/logger.h"

namespace cec_control {

namespace {

constexpr std::size_t MAX_CLIENT_CONNECTIONS = 10;
constexpr int LISTEN_BACKLOG = 16;
constexpr mode_t SOCKET_FILE_PERMISSIONS = 0660;
constexpr auto CLIENT_IDLE_TIMEOUT = std::chrono::seconds(60);

} // namespace

SocketServer::SocketServer(std::string socketPath, std::shared_ptr<ThreadPool> threadPool)
    : m_socketPath(std::move(socketPath)),
      m_threadPool(std::move(threadPool)) {}

SocketServer::~SocketServer() {
    stop();
}

void SocketServer::setCommandHandler(CommandHandler handler) {
    m_handler = std::move(handler);
}

bool SocketServer::start() {
    if (m_listener.valid()) {
        LOG_WARNING("Socket server already running");
        return true;
    }
    if (!m_threadPool) {
        LOG_ERROR("SocketServer::start: no thread pool was supplied");
        return false;
    }

    // The parent directory is provisioned by DaemonBootstrap. Verify that we
    // can actually write into it; surface a clear error if a packaging or
    // permissions regression has left the path unusable.
    if (auto slash = m_socketPath.find_last_of('/'); slash != std::string::npos) {
        std::string parent = m_socketPath.substr(0, slash);
        if (::access(parent.c_str(), W_OK) != 0) {
            LOG_ERROR("Socket directory not writable: ", parent, " (", std::strerror(errno), ")");
            return false;
        }
    }

    m_listener = UnixSocket::listen(m_socketPath, SOCKET_FILE_PERMISSIONS, LISTEN_BACKLOG);
    if (!m_listener.valid()) {
        return false;
    }

    LOG_INFO("Socket server listening on ", m_socketPath);
    return true;
}

void SocketServer::stop() {
    if (!m_listener.valid() && m_connections.size() == 0) {
        return;
    }

    LOG_INFO("Stopping socket server");

    // Close the listener first so no further accepts can land while we
    // drain the live connections. onReadable() is a no-op once the
    // listener is invalid.
    m_listener.reset();

    // Connection handlers exit when their sockets shut down. Manager blocks
    // until every one has returned.
    m_connections.shutdown();

    if (::unlink(m_socketPath.c_str()) < 0 && errno != ENOENT) {
        LOG_WARNING("Failed to unlink socket file ", m_socketPath, ": ",
                    std::strerror(errno));
    }

    LOG_INFO("Socket server stopped");
}

void SocketServer::onReadable() {
    if (!m_listener.valid()) return;

    // The listener is non-blocking (SOCK_NONBLOCK on the socket that
    // UnixSocket::listen returns); drain the accept queue until EAGAIN.
    while (true) {
        UnixSocket client = m_listener.accept();
        if (!client.valid()) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            if (errno == ECONNABORTED) continue;  // peer aborted mid-accept
            LOG_WARNING("accept() failed: ", std::strerror(errno));
            break;
        }

        // Per-connection wall-clock for an idle peer. Enforced by the kernel
        // via SO_RCVTIMEO on the accepted socket.
        if (!client.setIoTimeout(CLIENT_IDLE_TIMEOUT)) {
            LOG_WARNING("Failed to set idle timeout; dropping connection");
            continue;
        }

        auto conn = std::make_unique<Connection>(std::move(client), m_handler);
        if (!m_connections.add(std::move(conn), *m_threadPool, MAX_CLIENT_CONNECTIONS)) {
            LOG_WARNING("Connection limit reached; dropping new client");
        }
    }
}

} // namespace cec_control
