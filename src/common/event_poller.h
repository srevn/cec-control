#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace cec_control {

/**
 * A utility class that provides an abstraction over epoll functionality
 * for efficient event-based I/O.
 */
class EventPoller {
public:
    // Event types - similar to poll/epoll flags
    enum class Event : uint32_t {
        READ = 1,        // Data available to read
        WRITE = 2,       // Ready for write
        ERROR = 4,       // Error condition
        HANGUP = 8,      // Hang up
        INVALID = 16     // Invalid file descriptor
    };

    // Combined event types for common use cases
    static constexpr uint32_t ERROR_EVENTS = 
        static_cast<uint32_t>(Event::ERROR) | 
        static_cast<uint32_t>(Event::HANGUP) | 
        static_cast<uint32_t>(Event::INVALID);

    // Event data structure returned by wait()
    struct EventData {
        int fd;
        uint32_t events;
    };

    /**
     * Create a new event poller
     */
    EventPoller();
    
    /**
     * Destructor - cleans up epoll fd
     */
    ~EventPoller();
    
    /**
     * Add a file descriptor to the poller
     * @param fd The file descriptor to add
     * @param events The events to watch for (bitwise OR of Event values)
     * @return true if successful, false otherwise
     */
    bool add(int fd, uint32_t events);

    /**
     * Change the events watched on an already-added file descriptor. Used by
     * sd-bus integration, where the requested event mask changes every time
     * sd_bus_process() runs (outbox-empty drops POLLOUT, arrival of a reply
     * adds POLLIN, etc). Returns false if @p fd was not previously add()ed.
     */
    bool modify(int fd, uint32_t events);

    /**
     * Remove a previously-added file descriptor from the poller. Required for
     * clean unregistration when a subsystem's fd goes invalid (e.g., a bus
     * disconnect before reopening). Does not close the fd. Returns false if
     * the descriptor was not registered.
     */
    bool remove(int fd);

    /**
     * Wait for events on the added file descriptors.
     *
     * @param timeoutMs Timeout in milliseconds, -1 for indefinite.
     * @return - A populated vector when at least one fd is ready.
     *         - An empty vector on timeout or when an EINTR aborted the wait;
     *           callers should treat this as "no events, continue".
     *         - std::nullopt on an unrecoverable error (e.g. EBADF on the
     *           epoll fd). Callers must stop using this poller in that case.
     */
    std::optional<std::vector<EventData>> wait(int timeoutMs = -1);
    
    /**
     * Convert epoll events to EventPoller events
     * @param epollEvents Events from epoll_wait()
     * @return Equivalent EventPoller events
     */
    static uint32_t epollToEvents(uint32_t epollEvents);
    
    /**
     * Convert EventPoller events to epoll events
     * @param events EventPoller events
     * @return Equivalent epoll events
     */
    static uint32_t eventsToEpoll(uint32_t events);

private:
    int m_epollFd;
};

// Operators for working with Event enum as flags
inline EventPoller::Event operator|(EventPoller::Event lhs, EventPoller::Event rhs) {
    return static_cast<EventPoller::Event>(
        static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs)
    );
}

inline uint32_t operator|(uint32_t lhs, EventPoller::Event rhs) {
    return lhs | static_cast<uint32_t>(rhs);
}

inline uint32_t operator|(EventPoller::Event lhs, uint32_t rhs) {
    return static_cast<uint32_t>(lhs) | rhs;
}

inline uint32_t operator&(uint32_t lhs, EventPoller::Event rhs) {
    return lhs & static_cast<uint32_t>(rhs);
}

} // namespace cec_control