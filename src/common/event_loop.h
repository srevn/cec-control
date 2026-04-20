#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "event_poller.h"

namespace cec_control {

/**
 * Single-threaded dispatch loop over an EventPoller.
 *
 * Each registered file descriptor carries a handler invoked whenever the
 * poller reports activity on it. The loop itself holds no thread state —
 * construct, register every source, call run(). stop() from within a
 * handler or any main-thread context ends the loop at the next safe
 * point inside the dispatch batch.
 *
 * Thread-safety contract: all methods must be called on the main thread
 * (i.e. the thread that will call run()). Cross-thread wake-ups go
 * through a registered eventfd source — typically MainThreadWork.
 */
class EventLoop {
public:
    /**
     * Called when a registered fd becomes ready. @p events is the OR of
     * EventPoller::Event flags that fired. Handlers may call add, modify,
     * remove, and stop on the loop they were dispatched from.
     */
    using Handler = std::function<void(uint32_t events)>;

    EventLoop() = default;
    ~EventLoop() = default;

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /**
     * Register @p fd with the given @p events mask and @p handler. Fails if
     * @p fd is already registered, if @p handler is empty, or if the
     * underlying EventPoller add fails.
     */
    [[nodiscard]] bool add(int fd, uint32_t events, Handler handler);

    /**
     * Replace the event mask for an already-registered fd. Handler stays
     * the same. Fails if @p fd was not previously add()ed.
     */
    [[nodiscard]] bool modify(int fd, uint32_t events);

    /**
     * Unregister @p fd. Idempotent: removing an unknown fd is a no-op.
     * Does not close the descriptor. Safe to call from within that fd's
     * own handler.
     */
    void remove(int fd);

    /**
     * Run until stop() is observed or the poller reports an unrecoverable
     * error. Blocks the calling thread. Safe to call exactly once.
     */
    void run();

    /**
     * Request the loop to exit. Observed between handler invocations and
     * at the top of each poll cycle. Async-signal-safety is NOT provided;
     * signal-sourced shutdown must go through SignalSource / signalfd.
     */
    void stop() noexcept { m_stopRequested = true; }

private:
    EventPoller m_poller;
    std::unordered_map<int, Handler> m_handlers;
    bool m_stopRequested = false;
};

} // namespace cec_control
