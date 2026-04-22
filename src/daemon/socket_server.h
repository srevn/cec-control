#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "../common/event_loop.h"
#include "../common/messages.h"
#include "../common/timer_source.h"
#include "../common/unix_socket.h"

namespace cec_control {

/**
 * Unix-socket listener and per-session I/O driver, all on the main event
 * loop. Accepts connections, reads datagrams, invokes the command handler,
 * and writes responses — every syscall runs on the thread that drives the
 * loop.
 *
 * The per-session state machine (main thread only) is:
 *
 *   Reading (READ on)  ── recv ──▶  Processing (READ off, WRITE off)
 *                                        │ handler → reply(resp)
 *                                        ▼
 *                             ┌── send ──▶ Reading    (success)
 *                             └── EAGAIN ▶ Sending   (WRITE on, holds body)
 *                                        │ retry on WRITE-ready
 *                                        ▼
 *                                      Reading
 *
 * Sessions are keyed on a monotonic @c SessionId — never on the fd — so a
 * response queued by a worker that completes after the peer has
 * disconnected is quietly dropped rather than misrouted to a reused fd.
 *
 * Shutdown is a straight map clear: every session fd is removed from the
 * loop and closed by its @c UnixSocket destructor. There is no
 * cross-thread wait; any worker that completes after @c stop() posts a
 * reply closure into @c MainThreadWork whose queue is no longer drained,
 * and the closure is destructed unexecuted with the daemon.
 */
class SocketServer {
public:
    using SessionId = std::uint64_t;

    /**
     * Deliver one @c Message back to the originating session. Main thread
     * only; silently dropped if the session has closed. Worker threads
     * that produce a response must route the invocation through
     * @c MainThreadWork::post first — see @c CECDaemon::handleCommand
     * for the canonical pattern.
     */
    using ResponseSink = std::function<void(Message)>;

    /**
     * Invoked on the main thread for each parsed request. The handler
     * must call @p reply exactly once — either synchronously before
     * returning, or by transferring ownership of the sink to a deferred
     * continuation that calls it later. Calling @p reply more than once
     * per request results in a duplicate response; throwing after a
     * successful @p reply is a contract violation.
     */
    using CommandHandler = std::function<void(Message request, ResponseSink reply)>;

    /** Upper bound on simultaneous client sessions. Excess accepts close. */
    static constexpr std::size_t kMaxConnections = 10;

    /** Close a session after this long without activity on our side. */
    static constexpr auto kClientIdleTimeout = std::chrono::seconds(60);

    /** How often the main thread sweeps the session map for idle peers. */
    static constexpr auto kIdleSweepInterval = std::chrono::seconds(15);

    /**
     * @param loop        Non-owning reference; must outlive *this. Used
     *                    for the listener, the idle-sweep timer, and
     *                    per-session fd registration.
     * @param socketPath  Filesystem path of the listening socket.
     */
    SocketServer(EventLoop& loop, std::string socketPath);
    ~SocketServer();

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;
    SocketServer(SocketServer&&) = delete;
    SocketServer& operator=(SocketServer&&) = delete;

    /**
     * Open the listener, arm the idle sweep, and register both with the
     * event loop. Returns false on any step; a partial setup is rolled
     * back before the call returns.
     */
    [[nodiscard]] bool start();

    /** Close the listener and every session. Idempotent. */
    void stop();

    /** Install/replace the per-request handler. Install before @c start(). */
    void setCommandHandler(CommandHandler handler);

    /**
     * Send a response to an open session. No-op if the session has
     * closed. Must be called on the main thread.
     */
    void sendResponse(SessionId id, Message response);

private:
    struct Session;

    void onAcceptReady();
    void onSessionEvent(SessionId id, std::uint32_t events);
    void onIdleSweep();

    /** Read one datagram from @p session, parse, and invoke the handler. */
    void processRequest(SessionId id, Session& session);

    /** Retry a queued send for a session armed on WRITE. */
    [[nodiscard]] bool drainPendingSend(SessionId id);

    /** Remove a session from the loop and erase it. Idempotent. */
    void closeSession(SessionId id);

    /** Lookup helper. Returns null if the session has closed. */
    [[nodiscard]] Session* findSession(SessionId id) noexcept;

    /** Response sink closure for a given session. */
    [[nodiscard]] ResponseSink makeSink(SessionId id);

    EventLoop&     m_loop;
    std::string    m_socketPath;
    UnixSocket     m_listener;
    TimerSource    m_idleTimer;
    CommandHandler m_handler;

    std::unordered_map<SessionId, std::unique_ptr<Session>> m_sessions;
    SessionId m_nextId = 1;

    // Shared across all session reads; safe because reads are serialised
    // on the main thread. Avoids one allocation per dispatch.
    std::array<std::uint8_t, MAX_MESSAGE_SIZE> m_readBuffer{};
};

} // namespace cec_control
