#include "socket_server.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include "../common/event_poller.h"
#include "../common/logger.h"

namespace cec_control {

namespace {

constexpr int LISTEN_BACKLOG = 16;
constexpr mode_t SOCKET_FILE_PERMISSIONS = 0660;

constexpr std::uint32_t READ_BIT  = static_cast<std::uint32_t>(EventPoller::Event::READ);
constexpr std::uint32_t WRITE_BIT = static_cast<std::uint32_t>(EventPoller::Event::WRITE);

} // namespace

/**
 * Per-session state held entirely on the main thread.
 *
 * Invariants (maintained across every main-thread transition):
 *   - @c requestInFlight ⇒ READ is NOT in the epoll mask.
 *   - @c !pendingResponse.empty() ⇒ WRITE IS in the epoll mask.
 *   - At most one of the two is truthy at any moment.
 */
struct SocketServer::Session {
    Session(SessionId i, UnixSocket f, std::chrono::steady_clock::time_point t) noexcept
        : id(i), fd(std::move(f)), lastActivity(t) {}

    SessionId                              id;
    UnixSocket                             fd;
    std::chrono::steady_clock::time_point  lastActivity;
    std::vector<std::uint8_t>              pendingResponse;
    bool                                   requestInFlight = false;
};

SocketServer::SocketServer(EventLoop& loop, std::string socketPath)
    : m_loop(loop), m_socketPath(std::move(socketPath)) {}

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
    if (!m_idleTimer.valid()) {
        LOG_ERROR("Idle timer source not initialised");
        return false;
    }

    // The parent directory is provisioned by DaemonBootstrap. Verify that we
    // can actually write into it; surface a clear error if a packaging or
    // permissions regression has left the path unusable.
    if (auto slash = m_socketPath.find_last_of('/'); slash != std::string::npos) {
        const std::string parent = m_socketPath.substr(0, slash);
        if (::access(parent.c_str(), W_OK) != 0) {
            LOG_ERROR("Socket directory not writable: ", parent,
                      " (", std::strerror(errno), ")");
            return false;
        }
    }

    m_listener = UnixSocket::listen(m_socketPath, SOCKET_FILE_PERMISSIONS, LISTEN_BACKLOG);
    if (!m_listener.valid()) {
        return false;
    }

    // Register listener + idle sweep timer. On any failure we unwind through
    // stop(), which idempotently cleans up whichever pieces we committed.
    if (!m_loop.add(m_listener.get(), READ_BIT,
                    [this](std::uint32_t) { onAcceptReady(); })) {
        LOG_ERROR("Failed to register listener with event loop");
        stop();
        return false;
    }
    if (!m_loop.add(m_idleTimer.fd(), READ_BIT,
                    [this](std::uint32_t) { onIdleSweep(); })) {
        LOG_ERROR("Failed to register idle timer with event loop");
        stop();
        return false;
    }
    if (!m_idleTimer.armOnce(
            std::chrono::duration_cast<std::chrono::milliseconds>(kIdleSweepInterval))) {
        LOG_ERROR("Failed to arm idle sweep timer");
        stop();
        return false;
    }

    LOG_INFO("Socket server listening on ", m_socketPath);
    return true;
}

void SocketServer::stop() {
    if (!m_listener.valid() && m_sessions.empty()) {
        return;
    }

    LOG_INFO("Stopping socket server");

    // Idle timer first so it cannot fire while we are tearing the map down.
    if (m_idleTimer.valid()) {
        m_loop.remove(m_idleTimer.fd());
        m_idleTimer.disarm();
    }

    // Listener next so no new accepts can land during session teardown.
    if (m_listener.valid()) {
        m_loop.remove(m_listener.get());
        m_listener.reset();
    }

    // Every session fd must come out of the loop before its UnixSocket
    // destructor closes it; otherwise a future add() against a freshly
    // recycled fd could conflict with a stale epoll registration.
    for (auto& [_, session] : m_sessions) {
        m_loop.remove(session->fd.get());
    }
    m_sessions.clear();

    if (::unlink(m_socketPath.c_str()) < 0 && errno != ENOENT) {
        LOG_WARNING("Failed to unlink socket file ", m_socketPath, ": ",
                    std::strerror(errno));
    }

    LOG_INFO("Socket server stopped");
}

void SocketServer::onAcceptReady() {
    if (!m_listener.valid()) return;

    // The listener is non-blocking; drain the accept queue until EAGAIN.
    while (true) {
        UnixSocket client = m_listener.accept();
        if (!client.valid()) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR || errno == ECONNABORTED) continue;
            LOG_WARNING("accept() failed: ", std::strerror(errno));
            return;
        }
        if (m_sessions.size() >= kMaxConnections) {
            LOG_WARNING("Connection limit reached; dropping new client");
            continue;  // UnixSocket dtor closes the fresh fd
        }

        const SessionId id = m_nextId++;
        auto session = std::make_unique<Session>(
            id, std::move(client), std::chrono::steady_clock::now());
        const int fd = session->fd.get();

        if (!m_loop.add(fd, READ_BIT,
                        [this, id](std::uint32_t events) { onSessionEvent(id, events); })) {
            LOG_WARNING("Failed to register session ", id,
                        " with event loop; dropping");
            continue;  // session dtor closes fd
        }
        m_sessions.emplace(id, std::move(session));
    }
}

void SocketServer::onSessionEvent(SessionId id, std::uint32_t events) {
    // Service any queued send before attempting a read: a successful flush
    // frees the session back to Reading, which is the state a fresh event
    // cycle would expect to find.
    if (events & WRITE_BIT) {
        if (!drainPendingSend(id)) return;  // closed by drain
    }

    if (events & READ_BIT) {
        Session* s = findSession(id);
        if (s && !s->requestInFlight) {
            processRequest(id, *s);
            return;
        }
    }

    // Hang-up or error with nothing to read/write: the peer is gone.
    // A clean close delivers READ+HANGUP together and the READ branch
    // above handles it via recv returning 0 + closeSession.
    if (events & EventPoller::ERROR_EVENTS) {
        closeSession(id);
    }
}

void SocketServer::processRequest(SessionId id, Session& session) {
    ssize_t received = 0;
    do {
        received = ::recv(session.fd.get(), m_readBuffer.data(),
                          m_readBuffer.size(), MSG_TRUNC);
    } while (received < 0 && errno == EINTR);

    if (received == 0) {
        closeSession(id);
        return;
    }
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // spurious
        LOG_DEBUG("recv() failed for session ", id, ": ", std::strerror(errno));
        closeSession(id);
        return;
    }
    if (static_cast<std::size_t>(received) > m_readBuffer.size()) {
        // MSG_TRUNC exposed that the datagram was larger than our buffer.
        // Every legitimate peer uses MAX_MESSAGE_SIZE as its upper bound; a
        // larger frame is a protocol-level divergence (mismatched constant,
        // bespoke client, truncation probe) rather than a malformed message.
        LOG_WARNING("Oversized datagram from session ", id, ": ", received,
                    " bytes exceeds MAX_MESSAGE_SIZE=", m_readBuffer.size(),
                    "; closing session (protocol divergence)");
        closeSession(id);
        return;
    }

    auto request = Message::deserialize(m_readBuffer.data(),
                                        static_cast<std::size_t>(received));
    if (!request) {
        LOG_WARNING("Malformed message from session ", id, ", closing");
        closeSession(id);
        return;
    }

    session.requestInFlight = true;
    session.lastActivity    = std::chrono::steady_clock::now();
    if (!m_loop.modify(session.fd.get(), 0)) {
        LOG_WARNING("modify(mask=0) failed for session ", id);
        closeSession(id);
        return;
    }

    // From here the handler may close the session synchronously (a reply
    // that hits EPIPE, for instance). The reference @p session must not be
    // touched after the invocation returns — every subsequent access goes
    // through sendResponse → findSession.
    if (!m_handler) {
        sendResponse(id, Message(MessageType::RESP_ERROR));
        return;
    }
    try {
        m_handler(std::move(*request), makeSink(id));
    } catch (const std::exception& e) {
        LOG_ERROR("Handler threw for session ", id, ": ", e.what());
        sendResponse(id, Message(MessageType::RESP_ERROR));
    } catch (...) {
        LOG_ERROR("Handler threw non-std exception for session ", id);
        sendResponse(id, Message(MessageType::RESP_ERROR));
    }
}

bool SocketServer::drainPendingSend(SessionId id) {
    Session* s = findSession(id);
    if (!s) return false;

    ssize_t sent = 0;
    do {
        sent = ::send(s->fd.get(), s->pendingResponse.data(),
                      s->pendingResponse.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    if (sent >= 0) {
        // SOCK_SEQPACKET is all-or-nothing: success ⇒ whole datagram out.
        s->pendingResponse.clear();
        s->requestInFlight = false;
        s->lastActivity    = std::chrono::steady_clock::now();
        if (!m_loop.modify(s->fd.get(), READ_BIT)) {
            LOG_WARNING("modify(READ) failed after send for session ", id);
            closeSession(id);
            return false;
        }
        return true;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Level-triggered epoll fires WRITE again when the kernel buffer
        // drains. Stay in Sending until the retry succeeds.
        return true;
    }
    LOG_DEBUG("send() failed for session ", id, ": ", std::strerror(errno));
    closeSession(id);
    return false;
}

void SocketServer::sendResponse(SessionId id, Message response) {
    Session* s = findSession(id);
    if (!s) return;  // closed; drop silently

    auto bytes = response.serialize();
    ssize_t sent = 0;
    do {
        sent = ::send(s->fd.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);

    if (sent >= 0) {
        s->pendingResponse.clear();
        s->requestInFlight = false;
        s->lastActivity    = std::chrono::steady_clock::now();
        if (!m_loop.modify(s->fd.get(), READ_BIT)) {
            LOG_WARNING("modify(READ) failed after send for session ", id);
            closeSession(id);
        }
        return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        s->pendingResponse = std::move(bytes);
        if (!m_loop.modify(s->fd.get(), WRITE_BIT)) {
            LOG_WARNING("modify(WRITE) failed for session ", id);
            closeSession(id);
        }
        return;
    }
    LOG_DEBUG("send() failed for session ", id, ": ", std::strerror(errno));
    closeSession(id);
}

void SocketServer::onIdleSweep() {
    m_idleTimer.consume();

    const auto now = std::chrono::steady_clock::now();
    std::vector<SessionId> expired;
    for (const auto& [id, session] : m_sessions) {
        // Skip sessions with work in flight — "idle" is about the peer,
        // not about whatever the daemon is currently doing for them.
        if (session->requestInFlight) continue;
        if (now - session->lastActivity > kClientIdleTimeout) {
            expired.push_back(id);
        }
    }
    for (const SessionId id : expired) {
        LOG_DEBUG("Closing idle session ", id);
        closeSession(id);
    }

    if (!m_idleTimer.armOnce(
            std::chrono::duration_cast<std::chrono::milliseconds>(kIdleSweepInterval))) {
        LOG_WARNING("Failed to re-arm idle sweep timer");
    }
}

void SocketServer::closeSession(SessionId id) {
    auto it = m_sessions.find(id);
    if (it == m_sessions.end()) return;
    m_loop.remove(it->second->fd.get());
    m_sessions.erase(it);  // UnixSocket dtor closes the fd
}

SocketServer::Session* SocketServer::findSession(SessionId id) noexcept {
    auto it = m_sessions.find(id);
    return it == m_sessions.end() ? nullptr : it->second.get();
}

ResponseSink SocketServer::makeSink(SessionId id) {
    return [this, id](Message response) {
        sendResponse(id, std::move(response));
    };
}

} // namespace cec_control
