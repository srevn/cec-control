#pragma once

#include <vector>
#include <cstdint>

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
     * Modify the events to watch for a file descriptor
     * @param fd The file descriptor to modify
     * @param events The new events to watch for (bitwise OR of Event values)
     * @return true if successful, false otherwise
     */
    bool modify(int fd, uint32_t events);
    
    /**
     * Remove a file descriptor from the poller
     * @param fd The file descriptor to remove
     * @return true if successful, false otherwise
     */
    bool remove(int fd);
    
    /**
     * Wait for events on the added file descriptors
     * @param timeoutMs Timeout in milliseconds, -1 for indefinite
     * @return Vector of EventData for the file descriptors that have events
     */
    std::vector<EventData> wait(int timeoutMs = -1);
    
    /**
     * Convert poll events to EventPoller events
     * @param pollEvents Events from poll()
     * @return Equivalent EventPoller events
     */
    static uint32_t pollToEvents(short pollEvents);
    
    /**
     * Convert EventPoller events to poll events
     * @param events EventPoller events
     * @return Equivalent poll events
     */
    static short eventsToPoll(uint32_t events);
    
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