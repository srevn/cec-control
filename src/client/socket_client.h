#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "../common/messages.h"
#include "../common/unix_socket.h"

namespace cec_control {

/**
 * Single-shot blocking RPC client over a Unix SEQPACKET socket.
 *
 * Typical usage: construct, connect(), sendCommand(), destruct. The connection
 * is closed when the SocketClient goes out of scope.
 */
class SocketClient {
public:
    explicit SocketClient(std::string socketPath = {});

    ~SocketClient() = default;

    SocketClient(const SocketClient&) = delete;
    SocketClient& operator=(const SocketClient&) = delete;

    SocketClient(SocketClient&&) noexcept = default;
    SocketClient& operator=(SocketClient&&) noexcept = default;

    /** Establish the connection. Returns false if the daemon is unreachable. */
    bool connect();

    /**
     * Send a command and block until the daemon responds. Returns nullopt on
     * I/O error, timeout, or a malformed response.
     */
    std::optional<Message> sendCommand(const Message& command);

    bool isConnected() const noexcept { return m_socket.valid(); }

private:
    static constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(2);
    static constexpr auto IO_TIMEOUT = std::chrono::seconds(10);

    std::string m_socketPath;
    UnixSocket m_socket;
};

} // namespace cec_control
