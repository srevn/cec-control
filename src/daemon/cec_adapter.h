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
        bool systemAudioMode;
        CEC::cec_logical_addresses wakeDevices;
        CEC::cec_logical_addresses powerOffDevices;

        Options() :
            deviceName("CEC Control"),
            autoPowerOn(false),
            autoWakeAVR(false),
            activateSource(false),
            systemAudioMode(false) {
            wakeDevices.Clear();
            powerOffDevices.Clear();
        }
    };

    /**
     * Observer callbacks invoked by libcec on its internal thread.
     * Installed once at construction and immutable thereafter: libcec
     * reads the stored function objects without synchronisation, so
     * any late re-wiring would be a data race. Either member may be
     * empty — the adapter no-ops the corresponding event.
     */
    struct Callbacks {
        /** TV signalled CEC standby (libcec command thread). */
        std::function<void()> onTvStandby;
        /** libcec raised CEC_ALERT_CONNECTION_LOST (libcec alert thread). */
        std::function<void()> onConnectionLost;
    };

    /**
     * Create a new CEC adapter. Callbacks are install-once.
     */
    CECAdapter(Options options, Callbacks callbacks);

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
     * Sends InactiveSource message before closing
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
     * Issue a CEC "Set Stream Path" message to route the active source
     * to @p physicalAddress (0xNMPQ format; e.g. 0x1000 for HDMI 1 on
     * the TV). Preferred over INPUT_SELECT + number-keypress sequences
     * when the TV honours the message.
     * @return true if libcec reports the message was sent, false otherwise.
     */
    bool setStreamPath(uint16_t physicalAddress);

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
    // libCEC owns the ICECAdapter instance; ownership is released back to it
    // via CECDestroy() rather than `delete`. Using the default unique_ptr
    // deleter would invoke `delete` on a libCEC-allocated object, which is
    // outside the documented API contract.
    struct AdapterDeleter {
        void operator()(CEC::ICECAdapter* adapter) const noexcept {
            if (adapter) ::CECDestroy(adapter);
        }
    };
    using AdapterPtr = std::unique_ptr<CEC::ICECAdapter, AdapterDeleter>;

    // Configuration
    Options m_options;
    std::string m_portName;

    // libCEC adapter. m_callbacks is a plain non-owning struct whose address
    // we hand to libcec via m_config.callbacks; libcec treats the pointer as
    // borrowed (consistent with AdapterDeleter's CECDestroy note — libcec
    // never free()s anything we hand it). Value-initialised so every function
    // slot starts out nullptr; setupCallbacks() fills in the ones we use.
    AdapterPtr m_adapter;
    CEC::ICECCallbacks m_callbacks{};
    CEC::libcec_configuration m_config;

    // Coherent connection state. Every write happens under m_adapterMutex
    // except the CEC_ALERT_CONNECTION_LOST path inside cecAlertCallback,
    // which fires on libcec's internal thread and cannot take the mutex
    // without risking deadlock. That write is an advisory hint; the
    // authoritative state is always the value observed inside the mutex.
    // The type is atomic purely to make the unlocked write well-defined.
    std::atomic<bool> m_connected;

    // Thread safety
    mutable std::recursive_mutex m_adapterMutex;

    // Callbacks — install-once at construction; libcec reads them from
    // its internal threads without a lock. Never reassigned post-ctor.
    const std::function<void()> m_tvStandbyCallback;
    const std::function<void()> m_connectionLostCallback;

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
