#include "event_poller.h"
#include "logger.h"

#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <poll.h>

namespace cec_control {

EventPoller::EventPoller() {
    m_epollFd = epoll_create1(0);
    if (m_epollFd < 0) {
        LOG_ERROR("Failed to create epoll instance: ", strerror(errno));
    }
}

EventPoller::~EventPoller() {
    if (m_epollFd >= 0) {
        close(m_epollFd);
    }
}

bool EventPoller::add(int fd, uint32_t events) {
    if (m_epollFd < 0 || fd < 0) {
        return false;
    }

    struct epoll_event ev;
    ev.events = eventsToEpoll(events);
    ev.data.fd = fd;

    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        LOG_ERROR("Failed to add fd ", fd, " to epoll: ", strerror(errno));
        return false;
    }
    return true;
}

bool EventPoller::modify(int fd, uint32_t events) {
    if (m_epollFd < 0 || fd < 0) {
        return false;
    }

    struct epoll_event ev;
    ev.events = eventsToEpoll(events);
    ev.data.fd = fd;

    if (epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) < 0) {
        LOG_ERROR("Failed to modify fd ", fd, " in epoll: ", strerror(errno));
        return false;
    }
    return true;
}

bool EventPoller::remove(int fd) {
    if (m_epollFd < 0 || fd < 0) {
        return false;
    }

    // epoll_ctl_del ignores its event argument but Linux <2.6.9 required a
    // non-null pointer; pass a zero-initialised one for defensiveness.
    struct epoll_event ev{};
    if (epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, &ev) < 0) {
        // ENOENT = not registered. Callers often tolerate that (idempotent
        // cleanup paths); log at debug rather than error.
        if (errno == ENOENT) {
            LOG_DEBUG("remove(): fd ", fd, " was not registered with epoll");
            return false;
        }
        LOG_ERROR("Failed to remove fd ", fd, " from epoll: ", strerror(errno));
        return false;
    }
    return true;
}

std::optional<std::vector<EventPoller::EventData>> EventPoller::wait(int timeoutMs) {
    if (m_epollFd < 0) {
        return std::nullopt;
    }

    constexpr int kMaxEvents = 16;
    struct epoll_event events[kMaxEvents];

    int numEvents = epoll_wait(m_epollFd, events, kMaxEvents, timeoutMs);
    if (numEvents < 0) {
        if (errno == EINTR) {
            return std::vector<EventData>{};  // Transient: caller continues.
        }
        LOG_ERROR("epoll_wait failed: ", strerror(errno));
        return std::nullopt;
    }

    std::vector<EventData> result;
    result.reserve(numEvents);
    for (int i = 0; i < numEvents; ++i) {
        result.push_back({events[i].data.fd, epollToEvents(events[i].events)});
    }
    return result;
}

uint32_t EventPoller::epollToEvents(uint32_t epollEvents) {
    uint32_t events = 0;
    if (epollEvents & EPOLLIN) events |= static_cast<uint32_t>(Event::READ);
    if (epollEvents & EPOLLOUT) events |= static_cast<uint32_t>(Event::WRITE);
    if (epollEvents & EPOLLERR) events |= static_cast<uint32_t>(Event::ERROR);
    if (epollEvents & EPOLLHUP) events |= static_cast<uint32_t>(Event::HANGUP);
    if (epollEvents & EPOLLRDHUP) events |= static_cast<uint32_t>(Event::HANGUP);
    return events;
}

uint32_t EventPoller::eventsToEpoll(uint32_t events) {
    uint32_t epollEvents = 0;
    if (events & static_cast<uint32_t>(Event::READ)) epollEvents |= EPOLLIN;
    if (events & static_cast<uint32_t>(Event::WRITE)) epollEvents |= EPOLLOUT;
    if (events & static_cast<uint32_t>(Event::ERROR)) epollEvents |= EPOLLERR;
    if (events & static_cast<uint32_t>(Event::HANGUP)) epollEvents |= EPOLLHUP;
    // EPOLLRDHUP is useful to detect remote end disconnection without reading
    if (events & static_cast<uint32_t>(Event::READ)) epollEvents |= EPOLLRDHUP;
    return epollEvents;
}

} // namespace cec_control