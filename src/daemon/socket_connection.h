#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../common/messages.h"
#include "../common/unix_socket.h"

namespace cec_control {

class ThreadPool;

/**
 * One server-side client connection. Owns its socket FD (RAII). Reads one
 * request, invokes the handler, writes one response, repeat. Exits when the
 * peer disconnects, an I/O timeout fires, the handler produces a malformed
 * message, or stop() is called from another thread.
 */
class Connection {
public:
    using Handler = std::function<Message(const Message&)>;

    Connection(UnixSocket fd, Handler handler) noexcept;
    ~Connection() = default;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    /** Runs the request-response loop on the calling thread until stopped. */
    void run();

    /**
     * Unblock any in-flight recv/send and tell run() to exit on its next
     * iteration. Safe to call from a different thread than run(). Idempotent.
     */
    void stop() noexcept;

private:
    UnixSocket m_fd;
    Handler m_handler;
    std::atomic<bool> m_running{true};
};

/**
 * Owns the set of live Connections. Each connection runs as a task on the
 * supplied ThreadPool; when the task completes, the entry removes itself.
 *
 * shutdown() signals every connection to stop and waits for the map to drain.
 * No "activeClients"/"activeHandlers" duplication; the map is the single
 * source of truth for connection liveness.
 */
class ConnectionManager {
public:
    ConnectionManager() = default;
    ~ConnectionManager();

    ConnectionManager(const ConnectionManager&) = delete;
    ConnectionManager& operator=(const ConnectionManager&) = delete;

    /**
     * Register a new Connection and dispatch its run() onto @p pool.
     * Returns false if the count would exceed @p maxConnections, or if the
     * pool rejects the task — in either case the Connection is destroyed
     * (and its FD closed) before returning.
     */
    bool add(std::unique_ptr<Connection> conn, ThreadPool& pool, std::size_t maxConnections);

    /**
     * Signal every registered Connection to stop, then block until all of
     * them have exited. Safe to call multiple times.
     */
    void shutdown();

    std::size_t size() const;

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_emptyCv;
    std::unordered_map<uint64_t, std::shared_ptr<Connection>> m_entries;
    uint64_t m_nextId = 1;
};

} // namespace cec_control
