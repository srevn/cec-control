#include "unix_socket.h"
#include "logger.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace cec_control {

namespace {

bool fillAddr(sockaddr_un& addr, const std::string& path) {
    addr = sockaddr_un{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        LOG_ERROR("Unix socket path too long (", path.size(), " >= ",
                  sizeof(addr.sun_path), "): ", path);
        return false;
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    return true;
}

bool setBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        LOG_ERROR("fcntl(F_GETFL) failed: ", std::strerror(errno));
        return false;
    }
    if (::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        LOG_ERROR("fcntl(F_SETFL) failed: ", std::strerror(errno));
        return false;
    }
    return true;
}

/**
 * Probe whether a process is actively accepting on @p path.
 * Distinguishes stale socket files from live listeners.
 */
enum class ProbeResult { Live, Stale, Error };

ProbeResult probeExisting(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        LOG_WARNING("probe socket() failed: ", std::strerror(errno));
        return ProbeResult::Error;
    }

    sockaddr_un addr;
    if (!fillAddr(addr, path)) {
        ::close(fd);
        return ProbeResult::Error;
    }

    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    int savedErrno = errno;
    ::close(fd);

    if (rc == 0) {
        return ProbeResult::Live;
    }
    if (savedErrno == ECONNREFUSED || savedErrno == ENOENT) {
        return ProbeResult::Stale;
    }
    LOG_WARNING("probe connect() to ", path, " returned unexpected error: ",
                std::strerror(savedErrno));
    return ProbeResult::Error;
}

} // namespace

void UnixSocket::reset() noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void UnixSocket::shutdownBoth() noexcept {
    if (m_fd >= 0) {
        ::shutdown(m_fd, SHUT_RDWR);
    }
}

bool UnixSocket::setIoTimeout(std::chrono::milliseconds timeout) const {
    if (m_fd < 0) return false;

    struct timeval tv{};
    tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);

    if (::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
        ::setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        LOG_WARNING("setsockopt(SO_RCVTIMEO/SO_SNDTIMEO) failed: ",
                    std::strerror(errno));
        return false;
    }
    return true;
}

UnixSocket UnixSocket::listen(const std::string& path, mode_t perms, int backlog) {
    if (::access(path.c_str(), F_OK) == 0) {
        switch (probeExisting(path)) {
            case ProbeResult::Live:
                LOG_ERROR("Another process is already listening on ", path);
                return {};
            case ProbeResult::Stale:
                if (::unlink(path.c_str()) < 0 && errno != ENOENT) {
                    LOG_ERROR("Failed to remove stale socket ", path, ": ",
                              std::strerror(errno));
                    return {};
                }
                break;
            case ProbeResult::Error:
                // Conservative: don't unlink something we can't verify is stale.
                LOG_ERROR("Cannot verify state of existing socket at ", path,
                          "; refusing to proceed");
                return {};
        }
    }

    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: ", std::strerror(errno));
        return {};
    }
    UnixSocket sock(fd);

    sockaddr_un addr;
    if (!fillAddr(addr, path)) return {};

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("bind(", path, ") failed: ", std::strerror(errno));
        return {};
    }

    if (::chmod(path.c_str(), perms) < 0) {
        LOG_WARNING("chmod(", path, ") failed: ", std::strerror(errno));
        // Non-fatal: the socket is still usable, just with unexpected perms.
    }

    if (::listen(fd, backlog) < 0) {
        LOG_ERROR("listen() failed: ", std::strerror(errno));
        return {};
    }

    return sock;
}

UnixSocket UnixSocket::connect(const std::string& path, Deadline deadline) {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        LOG_ERROR("socket() failed: ", std::strerror(errno));
        return {};
    }
    UnixSocket sock(fd);

    sockaddr_un addr;
    if (!fillAddr(addr, path)) return {};

    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        // ENOENT and ECONNREFUSED are "daemon not running" — log at DEBUG so
        // repeated client invocations don't spam the log.
        if (errno == ENOENT || errno == ECONNREFUSED) {
            LOG_DEBUG("connect(", path, ") failed: ", std::strerror(errno));
        } else {
            LOG_ERROR("connect(", path, ") failed: ", std::strerror(errno));
        }
        return {};
    }

    if (rc < 0) {
        pollfd pfd{fd, POLLOUT, 0};
        int ready = ::poll(&pfd, 1, deadline.remainingMs());
        if (ready == 0) {
            LOG_ERROR("connect(", path, ") timed out");
            return {};
        }
        if (ready < 0) {
            LOG_ERROR("poll() during connect failed: ", std::strerror(errno));
            return {};
        }

        int soerr = 0;
        socklen_t soLen = sizeof(soerr);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soLen) < 0) {
            LOG_ERROR("getsockopt(SO_ERROR) failed: ", std::strerror(errno));
            return {};
        }
        if (soerr != 0) {
            if (soerr == ENOENT || soerr == ECONNREFUSED) {
                LOG_DEBUG("connect(", path, ") failed: ", std::strerror(soerr));
            } else {
                LOG_ERROR("connect(", path, ") failed: ", std::strerror(soerr));
            }
            return {};
        }
    }

    if (!setBlocking(fd)) return {};
    return sock;
}

UnixSocket UnixSocket::accept() const {
    if (m_fd < 0) return {};
    int fd = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) return {};
    return UnixSocket(fd);
}

} // namespace cec_control
