#include "socket_connection.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <utility>

#include "../common/logger.h"
#include "thread_pool.h"

namespace cec_control {

Connection::Connection(UnixSocket fd, Handler handler) noexcept
    : m_fd(std::move(fd)), m_handler(std::move(handler)) {}

void Connection::stop() noexcept {
    if (m_running.exchange(false)) {
        // Unblocks any recv()/send() already in flight on the handler thread.
        m_fd.shutdownBoth();
    }
}

void Connection::run() {
    std::array<uint8_t, MAX_MESSAGE_SIZE> buffer;

    while (m_running.load(std::memory_order_acquire)) {
        ssize_t received = ::recv(m_fd.get(), buffer.data(), buffer.size(), MSG_TRUNC);

        if (received == 0) {
            // Peer closed the connection cleanly.
            break;
        }
        if (received < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // SO_RCVTIMEO fired: the client has been idle longer than our
                // budget. Close the connection.
                LOG_DEBUG("Client idle timeout, closing connection");
                break;
            }
            // ECONNRESET, ENOTCONN after shutdownBoth(), and similar: end cleanly.
            break;
        }
        if (static_cast<std::size_t>(received) > buffer.size()) {
            // MSG_TRUNC exposed that the datagram was larger than our buffer.
            LOG_WARNING("Received oversized datagram (", received, " bytes)");
            break;
        }

        auto request = Message::deserialize(
            buffer.data(), static_cast<std::size_t>(received)
        );
        if (!request) {
            LOG_WARNING("Received malformed message, closing connection");
            break;
        }

        Message response = m_handler ? m_handler(*request)
                                     : Message(MessageType::RESP_ERROR);

        auto out = response.serialize();
        ssize_t sent = ::send(m_fd.get(), out.data(), out.size(), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
                // Peer went away mid-response; nothing to do.
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // SO_SNDTIMEO — peer is not reading. Abandon connection.
                LOG_WARNING("Send timed out, closing connection");
                break;
            }
            LOG_WARNING("send() failed: ", std::strerror(errno));
            break;
        }
    }
}

ConnectionManager::~ConnectionManager() {
    shutdown();
}

std::size_t ConnectionManager::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

bool ConnectionManager::add(
    std::unique_ptr<Connection> conn,
    ThreadPool& pool,
    std::size_t maxConnections
) {
    std::shared_ptr<Connection> shared(conn.release());
    uint64_t id;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_entries.size() >= maxConnections) {
            return false;
        }
        id = m_nextId++;
        m_entries.emplace(id, shared);
    }

    auto finalize = [this, id]() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.erase(id);
        if (m_entries.empty()) {
            m_emptyCv.notify_all();
        }
    };

    try {
        pool.submit([shared, finalize = std::move(finalize)]() mutable {
            try {
                shared->run();
            } catch (const std::exception& e) {
                LOG_ERROR("Connection handler exception: ", e.what());
            } catch (...) {
                LOG_ERROR("Connection handler threw non-std exception");
            }
            finalize();
        });
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to submit connection task: ", e.what());
        finalize();
        return false;
    }

    return true;
}

void ConnectionManager::shutdown() {
    // Copy out the live connections under the lock; signal them outside the
    // lock so finalize() (which needs the lock) isn't deadlocked if a
    // connection exits between our iteration and signalling.
    std::vector<std::shared_ptr<Connection>> live;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        live.reserve(m_entries.size());
        for (const auto& [_, conn] : m_entries) {
            live.push_back(conn);
        }
    }
    for (auto& conn : live) {
        conn->stop();
    }
    live.clear();  // drop our references so only the pool tasks hold them

    // Bounded wait: a handler stuck inside a libcec call, a throttler sleep,
    // or queued on the router mutex cannot be interrupted from here. If the
    // deadline fires, log the stragglers and return — the thread pool's
    // join() is the next backstop, and systemd's TimeoutStopSec + SIGKILL
    // the final one. 7s exceeds the throttler's worst-case retry budget
    // (~1.5s) with generous slack, and stays well under the default 30s
    // TimeoutStopSec.
    constexpr auto kShutdownDeadline = std::chrono::seconds(7);

    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_emptyCv.wait_for(lock, kShutdownDeadline,
                           [this] { return m_entries.empty(); })) {
        return;
    }

    LOG_WARNING("ConnectionManager::shutdown: ", m_entries.size(),
                " connection(s) still running after ",
                kShutdownDeadline.count(),
                "s; proceeding (pool join will absorb)");
    for (const auto& [id, _] : m_entries) {
        LOG_WARNING("  still-live connection id=", id);
    }
}

} // namespace cec_control
