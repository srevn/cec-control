#pragma once

#include <string>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <chrono>

#include "../common/messages.h"
#include "command_queue.h"
#include "cec_adapter.h"
#include "command_throttler.h"
#include "device_operations.h"
#include "thread_pool.h"

namespace cec_control {

class CECManager {
public:
    // Configuration options
    struct Options {
        bool scanDevicesAtStartup;
        uint32_t commandTimeoutMs;  // Default timeout for commands
        
        Options() 
            : scanDevicesAtStartup(true),
              commandTimeoutMs(5000) {}  // 5 second default timeout
    };

    // Constructor now takes a thread pool reference for background operations
    CECManager(Options options = Options(), std::shared_ptr<ThreadPool> threadPool = nullptr);
    ~CECManager();

    // Initialize the CEC adapter
    bool initialize();
    
    // Close and cleanup CEC adapter
    void shutdown();
    
    // Try to reconnect if connection is lost
    bool reconnect();
    
    // Process client command (synchronous version)
    Message processCommand(const Message& command);
    
    // Process client command asynchronously
    std::shared_ptr<CECOperation> processCommandAsync(const Message& command, 
                                                    uint32_t timeoutMs = 0);
    
    // Check if adapter is valid and ready for operations
    bool isAdapterValid() const;
    
    // Scan for connected CEC devices
    void scanDevices();

    /**
     * Send standby commands to devices specified in the powerOffDevices mask
     */
    bool standbyDevices();
    
    /**
     * Power on devices specified in the wakeDevices mask
     */
    bool powerOnDevices();

private:
    // Components
    std::unique_ptr<CommandQueue> m_commandQueue;
    std::shared_ptr<CECAdapter> m_adapter;
    std::shared_ptr<CommandThrottler> m_throttler;
    std::shared_ptr<DeviceOperations> m_deviceOps;
    
    // Options
    Options m_options;
    
    // Thread pool for background operations (may be shared with daemon)
    std::shared_ptr<ThreadPool> m_threadPool;

    // Internal command handler for the command queue
    Message handleCommand(const Message& command);
};

} // namespace cec_control
