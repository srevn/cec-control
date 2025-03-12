#include "protocol.h"

#include <cstring>
#include <algorithm>

namespace cec_control {

std::vector<uint8_t> Protocol::packMessage(const Message& msg) {
    // Serialize the message
    std::vector<uint8_t> serialized = msg.serialize();
    
    // Create the packet with header and checksum
    std::vector<uint8_t> packet;
    
    // Reserve space to avoid reallocations
    packet.reserve(serialized.size() + 6);
    
    // Add magic bytes for protocol identification
    packet.push_back('C');
    packet.push_back('E');
    packet.push_back('C');
    
    // Add payload size (2 bytes, little endian)
    uint16_t size = static_cast<uint16_t>(serialized.size());
    packet.push_back(size & 0xFF);
    packet.push_back((size >> 8) & 0xFF);
    
    // Add the serialized message
    packet.insert(packet.end(), serialized.begin(), serialized.end());
    
    // Calculate and add checksum
    uint16_t checksum = calculateChecksum(serialized);
    packet.push_back(checksum & 0xFF);
    packet.push_back((checksum >> 8) & 0xFF);
    
    return packet;
}

Message Protocol::unpackMessage(const std::vector<uint8_t>& data) {
    // Verify packet integrity
    if (data.size() < 8 || data[0] != 'C' || data[1] != 'E' || data[2] != 'C') {
        // Invalid packet format
        return Message();
    }
    
    // Extract payload size
    uint16_t size = static_cast<uint16_t>(data[3]) | (static_cast<uint16_t>(data[4]) << 8);
    
    // Check if we have enough data - Fix: Cast size+7 to size_t to match data.size() type
    if (data.size() < static_cast<size_t>(size) + 7) {
        // Incomplete packet
        return Message();
    }
    
    // Extract the payload
    std::vector<uint8_t> payload(data.begin() + 5, data.begin() + 5 + size);
    
    // Verify checksum
    uint16_t expected_checksum = calculateChecksum(payload);
    uint16_t received_checksum = static_cast<uint16_t>(data[5 + size]) | 
                                (static_cast<uint16_t>(data[6 + size]) << 8);
    
    if (expected_checksum != received_checksum) {
        // Checksum mismatch
        return Message();
    }
    
    // Deserialize the message
    return Message::deserialize(payload);
}

uint16_t Protocol::calculateChecksum(const std::vector<uint8_t>& data) {
    uint16_t checksum = 0;
    for (uint8_t byte : data) {
        checksum = (checksum + byte) & 0xFFFF;
    }
    return checksum;
}

bool Protocol::validateMessage(const std::vector<uint8_t>& data) {
    // Basic size and header check
    if (data.size() < 8 || data[0] != 'C' || data[1] != 'E' || data[2] != 'C') {
        return false;
    }
    
    // Extract payload size
    uint16_t size = static_cast<uint16_t>(data[3]) | (static_cast<uint16_t>(data[4]) << 8);
    
    // Check if we have enough data - Fix: Cast size+7 to size_t to match data.size() type
    if (data.size() < static_cast<size_t>(size) + 7) {
        return false;
    }
    
    // Extract the payload
    std::vector<uint8_t> payload(data.begin() + 5, data.begin() + 5 + size);
    
    // Verify checksum
    uint16_t expected_checksum = calculateChecksum(payload);
    uint16_t received_checksum = static_cast<uint16_t>(data[5 + size]) | 
                                (static_cast<uint16_t>(data[6 + size]) << 8);
    
    return expected_checksum == received_checksum;
}

// Implementation of Message serialization/deserialization
std::vector<uint8_t> Message::serialize() const {
    std::vector<uint8_t> result;
    
    // Reserve space to avoid reallocations
    result.reserve(data.size() + 2);
    
    // Add message type and device ID
    result.push_back(static_cast<uint8_t>(type));
    result.push_back(deviceId);
    
    // Add additional data if present
    result.insert(result.end(), data.begin(), data.end());
    
    return result;
}

Message Message::deserialize(const std::vector<uint8_t>& data) {
    if (data.size() < 2) {
        // Invalid data
        return Message();
    }
    
    // Extract message type and device ID
    MessageType msgType = static_cast<MessageType>(data[0]);
    uint8_t devId = data[1];
    
    // Extract additional data if present
    std::vector<uint8_t> additionalData;
    if (data.size() > 2) {
        additionalData.assign(data.begin() + 2, data.end());
    }
    
    return Message(msgType, devId, additionalData);
}

} // namespace cec_control
