#pragma once

#include <string>
#include <sys/types.h>
#include <utility>

#include "deadline.h"

namespace cec_control {

/**
 * Owning, move-only wrapper around a Unix-domain socket file descriptor.
 *
 * Closes the descriptor on destruction. All sockets created by the factories
 * below are AF_UNIX + SOCK_SEQPACKET + FD_CLOEXEC.
 */
class UnixSocket {
public:
    UnixSocket() noexcept = default;
    explicit UnixSocket(int fd) noexcept : m_fd(fd) {}

    ~UnixSocket() noexcept { reset(); }

    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;

    UnixSocket(UnixSocket&& other) noexcept
        : m_fd(std::exchange(other.m_fd, -1)) {}

    UnixSocket& operator=(UnixSocket&& other) noexcept {
        if (this != &other) {
            reset();
            m_fd = std::exchange(other.m_fd, -1);
        }
        return *this;
    }

    /** Closes the descriptor if valid and leaves the object in an empty state. */
    void reset() noexcept;

    /** Relinquishes ownership without closing the descriptor. */
    int release() noexcept { return std::exchange(m_fd, -1); }

    int get() const noexcept { return m_fd; }
    bool valid() const noexcept { return m_fd >= 0; }
    explicit operator bool() const noexcept { return valid(); }

    /** Half-closes both directions. Thread-safe relative to run-loop syscalls on the same FD. */
    void shutdownBoth() noexcept;

    /**
     * Listen on a Unix SEQPACKET socket at @p path with permissions @p perms.
     * If the path exists but no process is accepting on it, the stale file is removed.
     * If another process is accepting, listen fails and an invalid socket is returned.
     * Returns an invalid UnixSocket on any failure.
     */
    static UnixSocket listen(const std::string& path, mode_t perms, int backlog);

    /**
     * Connect to a Unix SEQPACKET socket at @p path. The returned socket is in
     * blocking mode with no I/O timeouts set. Honors @p deadline for the
     * connect syscall itself.
     */
    static UnixSocket connect(const std::string& path, Deadline deadline);

    /**
     * Accept a pending connection. Returns an invalid UnixSocket if the listener
     * is non-blocking and no connection is pending (errno == EAGAIN) or on error.
     * The returned client socket is in blocking mode; callers set I/O timeouts.
     */
    UnixSocket accept() const;

    /** Apply the same timeout to both SO_RCVTIMEO and SO_SNDTIMEO. */
    bool setIoTimeout(std::chrono::milliseconds timeout) const;

private:
    int m_fd = -1;
};

} // namespace cec_control
