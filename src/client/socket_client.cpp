#include "socket_client.h"

#include <array>
#include <cerrno>
#include <string>
#include <sys/socket.h>

#include "../common/system_paths.h"

namespace cec_control {

namespace {

ClientErrorKind classifyConnectErrno(int err) noexcept {
    switch (err) {
        case ENOENT:
        case ECONNREFUSED:
            return ClientErrorKind::DaemonUnavailable;
        case ETIMEDOUT:
            return ClientErrorKind::ConnectTimeout;
        default:
            return ClientErrorKind::ConnectFailed;
    }
}

} // namespace

SocketClient::SocketClient(std::string socketPath)
    : m_socketPath(socketPath.empty() ? SystemPaths::getSocketPath()
                                      : std::move(socketPath)) {}

std::optional<ClientError> SocketClient::connect() {
    m_socket = UnixSocket::connect(m_socketPath, Deadline::in(CONNECT_TIMEOUT));
    if (!m_socket.valid()) {
        const int err = errno;
        return ClientError{classifyConnectErrno(err), err, m_socketPath};
    }

    // Cap every subsequent send/recv in the kernel so the request path needs
    // no bespoke deadline plumbing. If we cannot install the timeout we drop
    // the socket: a request without a recv ceiling could hang forever.
    if (!m_socket.setIoTimeout(IO_TIMEOUT)) {
        const int err = errno;
        m_socket.reset();
        return ClientError{ClientErrorKind::ConnectFailed, err,
                           "could not set I/O timeout on socket"};
    }
    return std::nullopt;
}

SocketClient::SendResult SocketClient::sendCommand(const Message& command) {
    if (!m_socket.valid()) {
        return ClientError{ClientErrorKind::NotConnected, 0, m_socketPath};
    }

    const auto outBuf = command.serialize();
    const ssize_t sent = ::send(m_socket.get(), outBuf.data(), outBuf.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        return ClientError{ClientErrorKind::SendFailed, errno, ""};
    }
    if (static_cast<std::size_t>(sent) != outBuf.size()) {
        // SEQPACKET semantics: a successful send transfers the entire datagram
        // or none. A short return here would indicate a kernel anomaly.
        return ClientError{ClientErrorKind::SendFailed, 0,
                           "short send (" + std::to_string(sent) + "/" +
                           std::to_string(outBuf.size()) + ")"};
    }

    std::array<uint8_t, MAX_MESSAGE_SIZE> buffer;
    const ssize_t received = ::recv(m_socket.get(), buffer.data(), buffer.size(), MSG_TRUNC);
    if (received == 0) {
        return ClientError{ClientErrorKind::PeerClosed, 0, ""};
    }
    if (received < 0) {
        const int err = errno;
        const bool timedOut = (err == EAGAIN || err == EWOULDBLOCK);
        return ClientError{
            timedOut ? ClientErrorKind::ResponseTimeout : ClientErrorKind::ReceiveFailed,
            err, ""};
    }
    if (static_cast<std::size_t>(received) > buffer.size()) {
        return ClientError{ClientErrorKind::OversizedResponse, 0,
                           std::to_string(received) + " bytes"};
    }

    auto response = Message::deserialize(buffer.data(), static_cast<std::size_t>(received));
    if (!response) {
        return ClientError{ClientErrorKind::MalformedResponse, 0, ""};
    }
    return std::move(*response);
}

} // namespace cec_control
