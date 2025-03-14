#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include <string>

#include <libcec/cec.h>
#include "../common/logger.h"

namespace cec_control {

/**
 * Encapsulates low-level CEC adapter functionality
 */
class CECAdapter {
public:
    /**
     * Configuration options for the CEC adapter
     */
    struct Options {
        std::string deviceName;
        bool autoPowerOn;
        bool autoWakeAVR;
        bool activateSource;
        bool powerOffOnStandby;
        bool systemAudioMode;

        Options() : 
            deviceName("CEC Controller"),
            autoPowerOn(true),
            autoWakeAVR(true),
            activateSource(false),
            powerOffOnStandby(false),
            systemAudioMode(true) {}
    };

    /**
     * Create a new CEC adapter
     */
    CECAdapter(Options options = Options());
    
    /**
     * Destructor
     */
    ~CECAdapter();
    
    /**
     * Initialize the adapter
     * @return true if successful, false otherwise
     */
    bool initialize();
    
    /**
     * Shutdown the adapter
     */
    void shutdown();
    
    /**
     * Check if the adapter is connected
     * @return true if connected, false otherwise
     */
    bool isConnected() const;
    
    /**
     * Check if we have an adapter object (even if not connected)
     * @return true if adapter object exists, false otherwise
     */
    bool hasAdapter() const;
    
    /**
     * Get a reference to the raw libCEC adapter
     * @return pointer to the libCEC adapter, or nullptr if not initialized
     */
    CEC::ICECAdapter* getRawAdapter() const;
    
    /**
     * Send a power on command to a device
     * @param address logical address of the device
     * @return true if successful, false otherwise
     */
    bool powerOnDevice(CEC::cec_logical_address address);
    
    /**
     * Send a standby command to a device
     * @param address logical address of the device
     * @return true if successful, false otherwise
     */
    bool standbyDevice(CEC::cec_logical_address address);
    
    /**
     * Send a volume up command
     * @return true if successful, false otherwise
     */
    bool volumeUp();
    
    /**
     * Send a volume down command
     * @return true if successful, false otherwise
     */
    bool volumeDown();
    
    /**
     * Send a mute toggle command
     * @return true if successful, false otherwise
     */
    bool toggleMute();
    
    /**
     * Send a keypress to a device
     * @param address logical address of the device
     * @param key the key code to send
     * @param release whether this is a key release (true) or press (false)
     * @return true if successful, false otherwise
     */
    bool sendKeypress(CEC::cec_logical_address address, CEC::cec_user_control_code key, bool release);
    
    /**
     * Get the physical address of a logical device
     * @param address logical address of the device
     * @return physical address of the device
     */
    uint16_t getDevicePhysicalAddress(CEC::cec_logical_address address) const;
    
    /**
     * Check if a device is active
     * @param address logical address of the device
     * @return true if active, false otherwise
     */
    bool isDeviceActive(CEC::cec_logical_address address) const;
    
    /**
     * Get the power status of a device
     * @param address logical address of the device
     * @return power status of the device
     */
    CEC::cec_power_status getDevicePowerStatus(CEC::cec_logical_address address) const;
    
    /**
     * Get the OSD name of a device
     * @param address logical address of the device
     * @return OSD name of the device
     */
    std::string getDeviceOSDName(CEC::cec_logical_address address) const;
    
    /**
     * Get all active devices
     * @return active devices
     */
    CEC::cec_logical_addresses getActiveDevices() const;
    
    /**
     * Get the active source
     * @return logical address of the active source, or CECDEVICE_UNKNOWN if none
     */
    CEC::cec_logical_address getActiveSource() const;

private:
    // Configuration
    Options m_options;
    
    // libCEC adapter
    std::unique_ptr<CEC::ICECAdapter> m_adapter;
    CEC::libcec_configuration m_config;
    std::atomic<bool> m_connected;
    
    // Thread safety
    mutable std::mutex m_adapterMutex;
    
    // Set up CEC callbacks
    void setupCallbacks();
    
    // CEC callback handlers
    static void cecLogCallback(void *cbParam, const CEC::cec_log_message* message);
    static void cecCommandCallback(void *cbParam, const CEC::cec_command* command);
    static void cecAlertCallback(void *cbParam, const CEC::libcec_alert alert, const CEC::libcec_parameter param);
    static void cecMenuCallback(void *cbParam, const CEC::cec_menu_state state);

    // Helper to check if a CEC operation result indicates success
    // libCEC success codes can be 0 or 1 depending on the command
    bool isCecSuccess(int result) const {
        // For volume/audio commands, 1 means success, but 0 can also indicate success in some cases
        // where the command was sent but no acknowledgment is expected
        return result != -1;  // Only treat -1 as absolute failure
    }
};

} // namespace cec_control
