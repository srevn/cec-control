#include "socket_client.h"

#include <sys/socket.h>
#include <array>
#include <cerrno>
#include <cstring>

#include "../common/logger.h"
#include "../common/system_paths.h"

namespace cec_control {

SocketClient::SocketClient(std::string socketPath)
    : m_socketPath(socketPath.empty() ? SystemPaths::getSocketPath()
                                      : std::move(socketPath)) {}

bool SocketClient::connect() {
    m_socket = UnixSocket::connect(m_socketPath, Deadline::in(CONNECT_TIMEOUT));
    if (!m_socket.valid()) {
        return false;
    }

    // Cap every subsequent send/recv in the kernel, so the caller never has
    // to thread deadlines explicitly through the request path.
    if (!m_socket.setIoTimeout(IO_TIMEOUT)) {
        LOG_WARNING("Failed to set socket I/O timeout");
    }
    return true;
}

std::optional<Message> SocketClient::sendCommand(const Message& command) {
    if (!m_socket.valid()) {
        LOG_ERROR("Not connected to daemon");
        return std::nullopt;
    }

    auto outBuf = command.serialize();
    ssize_t sent = ::send(m_socket.get(), outBuf.data(), outBuf.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        LOG_ERROR("send() failed: ", std::strerror(errno));
        return std::nullopt;
    }
    // SEQPACKET is atomic: a successful send transfers the entire datagram or
    // none of it. A short return would indicate a kernel bug.
    if (static_cast<std::size_t>(sent) != outBuf.size()) {
        LOG_ERROR("Unexpected partial send on SEQPACKET (", sent, "/",
                  outBuf.size(), ")");
        return std::nullopt;
    }

    std::array<uint8_t, MAX_MESSAGE_SIZE> buffer;
    ssize_t received = ::recv(m_socket.get(), buffer.data(), buffer.size(), MSG_TRUNC);
    if (received == 0) {
        LOG_ERROR("Daemon closed connection");
        return std::nullopt;
    }
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            LOG_ERROR("Timed out waiting for response from daemon");
        } else {
            LOG_ERROR("recv() failed: ", std::strerror(errno));
        }
        return std::nullopt;
    }
    if (static_cast<std::size_t>(received) > buffer.size()) {
        LOG_ERROR("Received oversized response (", received, " bytes)");
        return std::nullopt;
    }

    auto msg = Message::deserialize(buffer.data(), static_cast<std::size_t>(received));
    if (!msg) {
        LOG_ERROR("Received malformed response from daemon");
    }
    return msg;
}

} // namespace cec_control
