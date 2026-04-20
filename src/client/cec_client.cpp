#include "cec_client.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <utility>

namespace cec_control {

namespace {

/** strerror() for an errno value, with a sensible fallback for the unset case. */
const char* strerrorOr(int err) noexcept {
    return err == 0 ? "operation failed" : std::strerror(err);
}

} // namespace

CECClient::CECClient(std::string socketPath)
    : m_socketClient(std::move(socketPath)) {}

int CECClient::execute(const Message& command) {
    if (auto err = m_socketClient.connect()) {
        renderConnectError(*err);
        return EXIT_FAILURE;
    }

    auto result = m_socketClient.sendCommand(command);
    if (auto* err = std::get_if<ClientError>(&result)) {
        renderTransportError(*err);
        return EXIT_FAILURE;
    }
    return renderResponse(std::get<Message>(result));
}

void CECClient::renderConnectError(const ClientError& err) const {
    switch (err.kind) {
        case ClientErrorKind::DaemonUnavailable:
            std::cerr << "Error: cec-control daemon is not running"
                      << " (socket: " << err.detail << ")\n";
            break;
        case ClientErrorKind::ConnectTimeout:
            std::cerr << "Error: timed out connecting to daemon"
                      << " (socket: " << err.detail << ")\n";
            break;
        case ClientErrorKind::ConnectFailed:
            std::cerr << "Error: failed to connect to daemon: "
                      << strerrorOr(err.errnoCode);
            if (!err.detail.empty()) {
                std::cerr << " (" << err.detail << ")";
            }
            std::cerr << '\n';
            break;
        default:
            // Defensive: if a new ClientErrorKind shows up at the connect
            // path it should still produce a comprehensible message.
            std::cerr << "Error: connect failed with unrecognised condition\n";
            break;
    }
}

void CECClient::renderTransportError(const ClientError& err) const {
    switch (err.kind) {
        case ClientErrorKind::NotConnected:
            std::cerr << "Error: not connected to daemon\n";
            break;
        case ClientErrorKind::SendFailed:
            std::cerr << "Error: failed to send command: "
                      << strerrorOr(err.errnoCode);
            if (!err.detail.empty()) std::cerr << " (" << err.detail << ")";
            std::cerr << '\n';
            break;
        case ClientErrorKind::PeerClosed:
            std::cerr << "Error: daemon closed the connection\n";
            break;
        case ClientErrorKind::ResponseTimeout:
            std::cerr << "Error: timed out waiting for daemon response\n";
            break;
        case ClientErrorKind::ReceiveFailed:
            std::cerr << "Error: failed to receive response: "
                      << strerrorOr(err.errnoCode) << '\n';
            break;
        case ClientErrorKind::OversizedResponse:
            std::cerr << "Error: daemon returned oversized response ("
                      << err.detail << ")\n";
            break;
        case ClientErrorKind::MalformedResponse:
            std::cerr << "Error: daemon returned a malformed response\n";
            break;
        default:
            std::cerr << "Error: unrecognised transport failure\n";
            break;
    }
}

int CECClient::renderResponse(const Message& response) const {
    if (response.type == MessageType::RESP_SUCCESS) {
        std::cout << "Command executed successfully\n";
        return EXIT_SUCCESS;
    }
    std::cerr << "Error: command failed\n";
    return EXIT_FAILURE;
}

} // namespace cec_control
