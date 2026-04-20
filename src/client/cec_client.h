#pragma once

#include <string>

#include "../common/messages.h"
#include "socket_client.h"

namespace cec_control {

/**
 * Top-level client adapter.
 *
 * Owns a SocketClient, drives one round-trip with the daemon, and renders
 * the outcome — success, daemon-side error, or transport failure — onto the
 * standard streams. Stdout carries the human-readable success line; stderr
 * carries every diagnostic so that pipelines consuming stdout see only the
 * positive result.
 */
class CECClient {
public:
    explicit CECClient(std::string socketPath);

    CECClient(const CECClient&) = delete;
    CECClient& operator=(const CECClient&) = delete;

    /**
     * Connect, send @p command, render the result. Returns a process exit
     * code: EXIT_SUCCESS only when the daemon acknowledged the command with
     * RESP_SUCCESS.
     */
    int execute(const Message& command);

private:
    void renderConnectError(const ClientError& err) const;
    void renderTransportError(const ClientError& err) const;
    int  renderResponse(const Message& response) const;

    SocketClient m_socketClient;
};

} // namespace cec_control
