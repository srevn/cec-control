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

    if (epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr) < 0) {
        // Only log error if it's not ENOENT or EBADF (fd already closed or removed)
        if (errno != ENOENT && errno != EBADF) {
            LOG_ERROR("Failed to remove fd ", fd, " from epoll: ", strerror(errno));
        }
        return false;
    }
    return true;
}

std::vector<EventPoller::EventData> EventPoller::wait(int timeoutMs) {
    if (m_epollFd < 0) {
        return {};
    }

    std::vector<EventData> result;
    const int MAX_EVENTS = 10;
    struct epoll_event events[MAX_EVENTS];

    int numEvents = epoll_wait(m_epollFd, events, MAX_EVENTS, timeoutMs);
    if (numEvents < 0) {
        if (errno != EINTR) {
            LOG_ERROR("epoll_wait failed: ", strerror(errno));
        }
        return {};
    }

    result.reserve(numEvents);
    for (int i = 0; i < numEvents; i++) {
        EventData data;
        data.fd = events[i].data.fd;
        data.events = epollToEvents(events[i].events);
        result.push_back(data);
    }

    return result;
}

uint32_t EventPoller::pollToEvents(short pollEvents) {
    uint32_t events = 0;
    if (pollEvents & POLLIN) events |= static_cast<uint32_t>(Event::READ);
    if (pollEvents & POLLOUT) events |= static_cast<uint32_t>(Event::WRITE);
    if (pollEvents & POLLERR) events |= static_cast<uint32_t>(Event::ERROR);
    if (pollEvents & POLLHUP) events |= static_cast<uint32_t>(Event::HANGUP);
    if (pollEvents & POLLNVAL) events |= static_cast<uint32_t>(Event::INVALID);
    return events;
}

short EventPoller::eventsToPoll(uint32_t events) {
    short pollEvents = 0;
    if (events & static_cast<uint32_t>(Event::READ)) pollEvents |= POLLIN;
    if (events & static_cast<uint32_t>(Event::WRITE)) pollEvents |= POLLOUT;
    if (events & static_cast<uint32_t>(Event::ERROR)) pollEvents |= POLLERR;
    if (events & static_cast<uint32_t>(Event::HANGUP)) pollEvents |= POLLHUP;
    if (events & static_cast<uint32_t>(Event::INVALID)) pollEvents |= POLLNVAL;
    return pollEvents;
}

uint32_t EventPoller::epollToEvents(uint32_t epollEvents) {
    uint32_t events = 0;
    if (epollEvents & EPOLLIN) events |= static_cast<uint32_t>(Event::READ);
    if (epollEvents & EPOLLOUT) events |= static_cast<uint32_t>(Event::WRITE);
    if (epollEvents & EPOLLERR) events |= static_cast<uint32_t>(Event::ERROR);
    if (epollEvents & EPOLLHUP) events |= static_cast<uint32_t>(Event::HANGUP);
    if (epollEvents & EPOLLRDHUP) events |= static_cast<uint32_t>(Event::HANGUP);
    // EPOLLNVAL doesn't exist, but we map to Event::INVALID for consistency with poll
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
    // Set edge-triggered mode for better performance
    epollEvents |= EPOLLET;
    return epollEvents;
}

} // namespace cec_control