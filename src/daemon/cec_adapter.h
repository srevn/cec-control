#pragma once

#include <memory>
#include <mutex>
#include <atomic>
#include <string>
#include <functional>

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
        CEC::cec_logical_addresses wakeDevices;
        CEC::cec_logical_addresses powerOffDevices;

        Options() :
            deviceName("CEC Control"),
            autoPowerOn(false),
            autoWakeAVR(false),
            activateSource(false),
            powerOffOnStandby(false),
            systemAudioMode(false) {
            wakeDevices.Clear();
            powerOffDevices.Clear();
        }
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
     * Initialize the libCEC library and detect hardware.
     * Must be called before openConnection().
     * @return true on success, false otherwise.
     */
    bool initialize();

    /**
     * Open a connection to the adapter
     * @return true if successful, false otherwise
     */
    bool openConnection();

    /**
     * Close the connection to the adapter
     */
    void closeConnection();

    /**
     * Reopen the connection to the adapter
     * @return true if successful, false otherwise
     */
    bool reopenConnection();

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
     * Configure the adapter with the given options.
     * Updates internal config and applies it to the adapter hardware.
     * @param options The configuration options to apply.
     * @return true if configuration was applied successfully, false otherwise.
     */
    bool configureAdapter(const Options& options);

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

    /**
     * Set whether the system should auto-suspend when the TV powers off
     * @param enabled Whether to enable or disable the feature
     */
    void setAutoStandby(bool enabled);

    /**
     * @brief Set a callback to be invoked when the TV signals standby
     * @param callback The function to call
     */
    void setOnTvStandbyCallback(std::function<void()> callback);

    /**
     * @brief Set a callback to be invoked when the CEC connection is lost
     * @param callback The function to call
     */
    void setConnectionLostCallback(std::function<void()> callback);

    /**
     * Send standby commands to configured devices
     * @param address The logical address to put in standby (CECDEVICE_BROADCAST uses powerOffDevices list)
     * @return True on success, false otherwise
     */
    bool standbyDevices(CEC::cec_logical_address address = CEC::CECDEVICE_BROADCAST);

    /**
     * Power on configured devices
     * @param address The logical address to power on (CECDEVICE_BROADCAST uses wakeDevices list)
     * @return True on success, false otherwise
     */
    bool powerOnDevices(CEC::cec_logical_address address = CEC::CECDEVICE_BROADCAST);

private:
    // Configuration
    Options m_options;
    std::string m_portName;

    // libCEC adapter
    std::unique_ptr<CEC::ICECAdapter> m_adapter;
    CEC::libcec_configuration m_config;
    std::atomic<bool> m_connected;

    // Thread safety
    mutable std::recursive_mutex m_adapterMutex;

    // Callbacks
    std::function<void()> m_tvStandbyCallback;
    std::function<void()> m_connectionLostCallback;

    // Set up CEC callbacks
    void setupCallbacks();

    /**
     * @brief Populates the libcec_configuration struct from our Options.
     * @param options The configuration options to use.
     */
    void populateConfigFromOptions(const Options& options);

    /**
     * @brief Detects available CEC adapter hardware.
     * @return true if an adapter was found, false otherwise.
     */
    bool detectAdapter();

    // CEC callback handlers
    static void cecLogCallback(void *cbParam, const CEC::cec_log_message* message);
    static void cecCommandCallback(void *cbParam, const CEC::cec_command* command);
    static void cecAlertCallback(void *cbParam, const CEC::libcec_alert alert, const CEC::libcec_parameter param);
};

} // namespace cec_control
