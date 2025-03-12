#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "messages.h"

namespace cec_control {

const std::string SOCKET_PATH = "/tmp/cec_control.sock";

class Protocol {
public:
    using MessageCallback = std::function<void(const Message&)>;
    
    Protocol() = default;
    virtual ~Protocol() = default;
    
    // Convert raw message to binary for transmission
    static std::vector<uint8_t> packMessage(const Message& msg);
    
    // Extract message from binary data
    static Message unpackMessage(const std::vector<uint8_t>& data);
    
    // Utility functions for protocol handling
    static uint16_t calculateChecksum(const std::vector<uint8_t>& data);
    static bool validateMessage(const std::vector<uint8_t>& data);
};

} // namespace cec_control