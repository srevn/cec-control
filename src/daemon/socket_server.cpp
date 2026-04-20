#include "socket_server.h"

#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <thread>
#include <utility>

#include "../common/event_poller.h"
#include "../common/logger.h"
#include "../common/system_paths.h"

namespace cec_control {

namespace {

constexpr unsigned int DEFAULT_THREAD_COUNT = 4;
constexpr unsigned int MAX_THREADS = 8;
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
    if (m_running.load()) {
        LOG_WARNING("Socket server already running");
        return true;
    }

    // Parent directory of the socket must exist and be writable. Permissions
    // on the socket file itself are handled inside UnixSocket::listen.
    auto slash = m_socketPath.find_last_of('/');
    if (slash != std::string::npos) {
        std::string parent = m_socketPath.substr(0, slash);
        if (!SystemPaths::createDirectories(parent) || ::access(parent.c_str(), W_OK) != 0) {
            LOG_ERROR("Socket directory missing or not writable: ", parent);
            return false;
        }
    }

    m_listener = UnixSocket::listen(m_socketPath, SOCKET_FILE_PERMISSIONS, LISTEN_BACKLOG);
    if (!m_listener.valid()) {
        return false;
    }

    m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeFd < 0) {
        LOG_ERROR("eventfd() failed: ", std::strerror(errno));
        m_listener.reset();
        return false;
    }

    if (!m_threadPool) {
        unsigned int threadCount = std::min(std::thread::hardware_concurrency(), MAX_THREADS);
        if (threadCount == 0) threadCount = DEFAULT_THREAD_COUNT;
        m_threadPool = std::make_shared<ThreadPool>(threadCount);
        m_threadPool->start();
        m_ownsThreadPool = true;
        LOG_INFO("Created thread pool with ", threadCount, " workers");
    }

    m_running.store(true);
    m_acceptThread = std::thread(&SocketServer::acceptLoop, this);

    LOG_INFO("Socket server started on ", m_socketPath);
    return true;
}

bool SocketServer::wakeupAcceptLoop() noexcept {
    if (m_wakeFd < 0) return false;
    uint64_t one = 1;
    ssize_t n = ::write(m_wakeFd, &one, sizeof(one));
    return n == sizeof(one);
}

void SocketServer::stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    LOG_INFO("Stopping socket server");

    if (!wakeupAcceptLoop()) {
        // Last-resort fallback: half-close the listener to jolt accept()/poll().
        m_listener.shutdownBoth();
    }

    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }

    if (m_wakeFd >= 0) {
        ::close(m_wakeFd);
        m_wakeFd = -1;
    }

    // Connection handlers exit when their sockets shut down. Manager blocks
    // until every one has returned.
    m_connections.shutdown();

    m_listener.reset();
    if (::unlink(m_socketPath.c_str()) < 0 && errno != ENOENT) {
        LOG_WARNING("Failed to unlink socket file ", m_socketPath, ": ",
                    std::strerror(errno));
    }

    if (m_ownsThreadPool && m_threadPool) {
        m_threadPool->shutdown();
        m_threadPool.reset();
        m_ownsThreadPool = false;
    }

    LOG_INFO("Socket server stopped");
}

void SocketServer::acceptLoop() {
    EventPoller poller;
    if (!poller.add(m_listener.get(), static_cast<uint32_t>(EventPoller::Event::READ)) ||
        !poller.add(m_wakeFd, static_cast<uint32_t>(EventPoller::Event::READ))) {
        LOG_ERROR("Failed to register fds with event poller; accept loop exiting");
        return;
    }

    while (m_running.load(std::memory_order_acquire)) {
        auto events = poller.wait(-1);

        bool shouldExit = false;
        bool hasAccept = false;

        for (const auto& ev : events) {
            if (ev.fd == m_wakeFd) {
                shouldExit = true;
                break;
            }
            if (ev.fd == m_listener.get()) {
                if (ev.events & EventPoller::ERROR_EVENTS) {
                    // The listen socket is unrecoverable. Log and exit; the
                    // supervising service (systemd) can restart the daemon.
                    LOG_ERROR("Listener socket reported error; exiting accept loop");
                    shouldExit = true;
                    break;
                }
                if (ev.events & static_cast<uint32_t>(EventPoller::Event::READ)) {
                    hasAccept = true;
                }
            }
        }

        if (shouldExit) break;
        if (!hasAccept) continue;

        // Drain the accept queue while we're here. The listener is non-blocking
        // so we get EAGAIN when the queue empties.
        while (m_running.load(std::memory_order_acquire)) {
            UnixSocket client = m_listener.accept();
            if (!client.valid()) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                if (errno == ECONNABORTED) continue;  // peer aborted before we processed
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
                // conn was either already-destroyed (add took ownership and
                // destroyed on rejection) or never constructed further. FD
                // closes via RAII.
            }
        }
    }

    LOG_DEBUG("Accept loop exiting");
}

} // namespace cec_control
