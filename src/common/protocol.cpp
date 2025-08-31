#include "protocol.h"
#include "logger.h"
#include "buffer_manager.h"

#include <cstring>
#include <algorithm>

namespace cec_control {

std::vector<uint8_t> Protocol::packMessage(const Message& msg) {
    // Use a buffer from the pool for improved efficiency
    auto& pool = BufferPoolManager::getInstance().getPool(256);  // Typical message size
    auto buffer = pool.acquireBuffer();
    buffer->clear();
    
    // Add magic bytes
    buffer->push_back('C');
    buffer->push_back('E');
    buffer->push_back('C');
    
    // Placeholder for size (will be filled in later)
    buffer->push_back(0);
    buffer->push_back(0);
    
    // Add the serialized message
    std::vector<uint8_t> serialized = msg.serialize();
    buffer->insert(buffer->end(), serialized.begin(), serialized.end());
    
    // Calculate size and update size field
    uint16_t dataSize = buffer->size() - 5;  // Exclude magic bytes and size field
    (*buffer)[3] = dataSize & 0xff;
    (*buffer)[4] = (dataSize >> 8) & 0xff;
    
    // Create a vector from the payload only for the checksum calculation
    uint16_t checksum = calculateChecksum(buffer->data() + 5, dataSize);
    buffer->push_back(checksum & 0xff);
    buffer->push_back((checksum >> 8) & 0xff);
    
    // Create a return buffer with just the right size
    std::vector<uint8_t> result(*buffer);
    
    // Return the buffer to the pool
    pool.releaseBuffer(std::move(buffer));
    
    return result;
}

Message Protocol::unpackMessage(const std::vector<uint8_t>& data) {
    return unpackMessage(data.data(), data.size());
}

Message Protocol::unpackMessage(const uint8_t* data, size_t len) {
    // Verify packet integrity
    if (!validateMessage(data, len)) {
        return Message();
    }
    
    // Extract payload size
    uint16_t size = static_cast<uint16_t>(data[3]) | (static_cast<uint16_t>(data[4]) << 8);
    
    // Extract the payload
    std::vector<uint8_t> payload(data + 5, data + 5 + size);
    
    // Deserialize the message
    return Message::deserialize(payload);
}

uint16_t Protocol::calculateChecksum(const std::vector<uint8_t>& data) {
    return calculateChecksum(data.data(), data.size());
}

uint16_t Protocol::calculateChecksum(const uint8_t* data, size_t len) {
    uint16_t checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        checksum = (checksum + data[i]) & 0xFFFF;
    }
    return checksum;
}

bool Protocol::validateMessage(const std::vector<uint8_t>& data) {
    return validateMessage(data.data(), data.size());
}

bool Protocol::validateMessage(const uint8_t* data, size_t len) {
    // Basic size and header check
    if (len < 7) { // 5 header + 2 checksum
        return false;
    }
    if (data[0] != 'C' || data[1] != 'E' || data[2] != 'C') {
        return false;
    }
    
    // Extract payload size
    uint16_t size = static_cast<uint16_t>(data[3]) | (static_cast<uint16_t>(data[4]) << 8);
    
    // Check if the length matches the size in the header
    if (len != static_cast<size_t>(size) + 7) {
        return false;
    }
    
    // Verify checksum
    uint16_t expected_checksum = calculateChecksum(data + 5, size);
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
