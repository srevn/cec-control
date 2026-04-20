#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <variant>

#include "../common/messages.h"
#include "../common/unix_socket.h"

namespace cec_control {

/**
 * Categorises every failure that the socket client can encounter. The owner
 * (CECClient) maps these to user-facing messages; the transport itself never
 * formats text.
 */
enum class ClientErrorKind {
    DaemonUnavailable,    // ENOENT or ECONNREFUSED on connect
    ConnectTimeout,       // connect did not complete within the budget
    ConnectFailed,        // any other connect-time failure
    NotConnected,         // sendCommand called before a successful connect
    SendFailed,           // send(2) returned an error or short transfer
    PeerClosed,           // recv returned 0 mid-exchange
    ResponseTimeout,      // recv timed out (idle peer)
    ReceiveFailed,        // any other recv-time failure
    OversizedResponse,    // datagram larger than our buffer
    MalformedResponse,    // wire format was rejected by the parser
};

/**
 * Structured failure value. errnoCode is 0 when the failure was not produced
 * by a syscall; detail is a free-form context string (socket path, byte
 * count, etc.) suitable for inclusion in user-visible diagnostics.
 */
struct ClientError {
    ClientErrorKind kind;
    int             errnoCode = 0;
    std::string     detail;
};

/**
 * Single-shot blocking RPC client over a Unix SEQPACKET socket.
 *
 * Connect once via connect(); each call to sendCommand() reuses that
 * connection. Failures are surfaced as ClientError values rather than
 * logged: client renderers (CECClient) translate them into user output,
 * which keeps stdout/stderr free of stray diagnostic lines.
 */
class SocketClient {
public:
    using SendResult = std::variant<Message, ClientError>;

    explicit SocketClient(std::string socketPath = {});

    SocketClient(const SocketClient&) = delete;
    SocketClient& operator=(const SocketClient&) = delete;
    SocketClient(SocketClient&&) noexcept = default;
    SocketClient& operator=(SocketClient&&) noexcept = default;

    /** Establish the connection. Returns nullopt on success. */
    std::optional<ClientError> connect();

    /** Send a command and block until the daemon responds. */
    SendResult sendCommand(const Message& command);

    bool isConnected() const noexcept { return m_socket.valid(); }
    const std::string& socketPath() const noexcept { return m_socketPath; }

private:
    static constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(2);
    static constexpr auto IO_TIMEOUT      = std::chrono::seconds(10);

    std::string m_socketPath;
    UnixSocket  m_socket;
};

} // namespace cec_control
