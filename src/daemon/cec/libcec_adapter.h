#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <libcec/cec.h>

#include "../../common/logger.h"
#include "adapter_interface.h"

namespace cec_control {

/**
 * @class LibCecAdapter
 * @brief Concrete @c ICecAdapter implementation backed by libcec.
 *
 * Every libcec API surface (the @c CEC::ICECAdapter handle, the
 * @c ICECCallbacks struct, the @c libcec_configuration) is confined to
 * this class. Consumers elsewhere in the daemon talk to
 * @c ICecAdapter and never see libcec directly.
 */
class LibCecAdapter final : public ICecAdapter {
public:
    /**
     * Configuration options for the libcec backend. Consumed once at
     * construction and immutable thereafter; runtime-mutable knobs
     * (like auto-standby) live on @c CommandRouter.
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
     * Construct a libcec-backed adapter. Callbacks are install-once
     * (see @c ICecAdapter::Callbacks).
     */
    LibCecAdapter(Options options, Callbacks callbacks);
    ~LibCecAdapter() override;

    LibCecAdapter(const LibCecAdapter&) = delete;
    LibCecAdapter& operator=(const LibCecAdapter&) = delete;

    // Lifecycle ---------------------------------------------------------
    [[nodiscard]] bool initialize() override;
    [[nodiscard]] bool openConnection() override;
    void closeConnection() override;
    [[nodiscard]] bool reopenConnection() override;
    [[nodiscard]] bool isConnected() const override;

    // Commands ----------------------------------------------------------
    [[nodiscard]] bool powerOnDevice(CEC::cec_logical_address address) override;
    [[nodiscard]] bool standbyDevice(CEC::cec_logical_address address) override;
    [[nodiscard]] bool volumeUp() override;
    [[nodiscard]] bool volumeDown() override;
    [[nodiscard]] bool toggleMute() override;
    [[nodiscard]] bool sendKeypress(CEC::cec_logical_address address,
                                    CEC::cec_user_control_code key,
                                    bool release) override;
    [[nodiscard]] bool setStreamPath(uint16_t physicalAddress) override;

    // Queries -----------------------------------------------------------
    [[nodiscard]] uint16_t getDevicePhysicalAddress(
        CEC::cec_logical_address address) const override;
    [[nodiscard]] bool isDeviceActive(
        CEC::cec_logical_address address) const override;
    [[nodiscard]] CEC::cec_power_status getDevicePowerStatus(
        CEC::cec_logical_address address) const override;
    [[nodiscard]] std::string getDeviceOSDName(
        CEC::cec_logical_address address) const override;
    [[nodiscard]] CEC::cec_logical_addresses getActiveDevices() const override;
    [[nodiscard]] CEC::cec_logical_address getActiveSource() const override;

    // Broadcast-style helpers ------------------------------------------
    [[nodiscard]] bool standbyDevices(
        CEC::cec_logical_address address = CEC::CECDEVICE_BROADCAST) override;
    [[nodiscard]] bool powerOnDevices(
        CEC::cec_logical_address address = CEC::CECDEVICE_BROADCAST) override;

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

    // Serialises libcec access and writes to m_connected. A plain mutex
    // is sufficient: every public member function enters the lock exactly
    // once, and reopenConnection() inlines its close/destroy sequence
    // rather than re-entering closeConnection(). If you add a new method
    // that needs to run under this lock, build a private *Locked() helper
    // — do not promote the mutex back to std::recursive_mutex.
    mutable std::mutex m_adapterMutex;

    // Callbacks — install-once at construction; libcec reads them from
    // its internal threads without a lock. Never reassigned post-ctor.
    const std::function<void()> m_tvStandbyCallback;
    const std::function<void()> m_connectionLostCallback;

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
