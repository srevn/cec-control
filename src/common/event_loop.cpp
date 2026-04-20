#include "event_loop.h"
#include "logger.h"

#include <utility>

namespace cec_control {

bool EventLoop::add(int fd, uint32_t events, Handler handler) {
    if (fd < 0 || !handler) {
        return false;
    }
    if (m_handlers.find(fd) != m_handlers.end()) {
        LOG_ERROR("EventLoop::add: fd ", fd, " already registered");
        return false;
    }
    if (!m_poller.add(fd, events)) {
        return false;
    }
    m_handlers.emplace(fd, std::move(handler));
    return true;
}

bool EventLoop::modify(int fd, uint32_t events) {
    if (m_handlers.find(fd) == m_handlers.end()) {
        LOG_ERROR("EventLoop::modify: fd ", fd, " not registered");
        return false;
    }
    return m_poller.modify(fd, events);
}

void EventLoop::remove(int fd) {
    if (m_handlers.erase(fd) == 0) {
        return;  // Not registered; nothing to do.
    }
    // Best-effort: the fd was registered at least once, so remove from
    // the poller. ENOENT (already detached) is tolerated.
    (void)m_poller.remove(fd);
}

void EventLoop::run() {
    while (!m_stopRequested) {
        auto events = m_poller.wait(-1);
        if (!events) {
            LOG_ERROR("EventLoop: poller failed; exiting");
            break;
        }
        if (m_stopRequested) break;

        for (const auto& ev : *events) {
            // Copy the handler out — if it removes itself from the map
            // during the call, the underlying std::function would otherwise
            // be destroyed mid-invocation.
            auto it = m_handlers.find(ev.fd);
            if (it == m_handlers.end()) {
                // Handler may have been removed by an earlier dispatch in
                // this same batch. Silently skip the stale event.
                continue;
            }
            Handler handler = it->second;
            handler(ev.events);
            if (m_stopRequested) break;
        }
    }
    m_stopRequested = false;  // Reset so run() could be called again.
}

} // namespace cec_control
