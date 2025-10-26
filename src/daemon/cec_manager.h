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

/**
 * @class CECManager
 * @brief Manages CEC device operations and command processing
 * 
 * Provides a high-level interface for CEC device control, command processing,
 * and adapter management. Handles reconnection logic and command throttling.
 */
class CECManager {
public:
    /**
     * @brief Configuration options for CEC Manager
     */
    struct Options {
        bool scanDevicesAtStartup;
        uint32_t commandTimeoutMs;  // Default timeout for commands
        
        Options() 
            : scanDevicesAtStartup(true),
              commandTimeoutMs(5000) {}  // 5 second default timeout
    };

    /**
     * @brief Constructor
     * @param options Configuration options
     * @param threadPool Optional thread pool for background operations
     */
    CECManager(Options options = Options(), std::shared_ptr<ThreadPool> threadPool = nullptr);
    
    /**
     * @brief Destructor
     */
    ~CECManager();

    /**
     * @brief Initialize the CEC adapter
     * @return true if successful, false otherwise
     */
    bool initialize();
    
    /**
     * @brief Close and cleanup CEC adapter
     */
    void shutdown();
    
    /**
     * @brief Try to reconnect if connection is lost
     * @param afterWake Set to true when reconnecting after system wake
     * @return true if reconnected successfully, false otherwise
     */
    bool reconnect(bool afterWake = false);
    
    /**
     * @brief Set a callback to be invoked when the CEC connection is lost
     * @param callback The function to call
     */
    void setConnectionLostCallback(std::function<void()> callback);
    
    /**
     * @brief Set a callback to be invoked when system suspend is requested
     * @param callback The function to call, should return true if suspend was successful
     */
    void setSuspendCallback(std::function<bool()> callback);

    /**
     * @brief Process client command synchronously
     * @param command Command message to process
     * @return Response message
     */
    Message processCommand(const Message& command);
    
    /**
     * @brief Process client command asynchronously
     * @param command Command message to process
     * @param timeoutMs Command timeout in milliseconds (0 = use default)
     * @return Operation handle that can be waited on
     */
    std::shared_ptr<CECOperation> processCommandAsync(const Message& command, 
                                                    uint32_t timeoutMs = 0);
    
    /**
     * @brief Check if adapter is valid and ready for operations
     * @return true if adapter is connected and operational
     */
    bool isAdapterValid() const;
    
    /**
     * @brief Scan for connected CEC devices
     */
    void scanDevices();

    /**
     * @brief Send standby commands to devices specified in the powerOffDevices mask
     * @return true if commands were sent successfully
     */
    bool standbyDevices();
    
    /**
     * @brief Power on devices specified in the wakeDevices mask
     * @return true if commands were sent successfully
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

    // Mutex for synchronizing high-level manager operations
    mutable std::mutex m_managerMutex;

    // Reconnection failure counter
    int m_reconnectFailures = 0;
    
    // Suspend callback
    std::function<bool()> m_suspendCallback;

    /**
     * @brief Internal command handler for the command queue
     * @param command Command message to handle
     * @return Response message
     */
    Message handleCommand(const Message& command);
};

} // namespace cec_control
