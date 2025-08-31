#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

#include "messages.h"

namespace cec_control {

class Protocol {
public:
    using MessageCallback = std::function<void(const Message&)>;
    
    Protocol() = default;
    virtual ~Protocol() = default;
    
    // Convert raw message to binary for transmission
    static std::vector<uint8_t> packMessage(const Message& msg);
    
    // Extract message from binary data
    static Message unpackMessage(const std::vector<uint8_t>& data);
    static Message unpackMessage(const uint8_t* data, size_t len);
    
    // Utility functions for protocol handling
    static uint16_t calculateChecksum(const std::vector<uint8_t>& data);
    static uint16_t calculateChecksum(const uint8_t* data, size_t len);
    static bool validateMessage(const std::vector<uint8_t>& data);
    static bool validateMessage(const uint8_t* data, size_t len);
};

} // namespace cec_control
