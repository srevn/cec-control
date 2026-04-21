#pragma once

#include <atomic>
#include <functional>
#include <memory>
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
 * @c ICECCallbacks struct, the @c libcec_configuration) is confined
 * to this class. Consumers elsewhere in the daemon talk to
 * @c ICecAdapter and never see libcec directly.
 *
 * ## Threading contract
 *
 * Exactly one thread — the @c AdapterWorker thread that owns this
 * instance — is allowed to invoke the public lifecycle / command /
 * query methods. There is no internal lock. The only cross-thread
 * field is @c m_connected, written by libcec's alert thread in the
 * @c CEC_ALERT_CONNECTION_LOST path and read as a cheap hint by the
 * worker via @c isConnected. An atomic suffices.
 *
 * ## Callback threads
 *
 * The three libcec callbacks (log, command-received, alert) fire on
 * libcec's internal threads, not on the owning worker. They must be
 * thread-safe and non-blocking; the implementations here only log,
 * invoke thread-safe user-supplied callbacks (install-once at
 * construction, never reassigned), or write the atomic
 * @c m_connected. They MUST NOT call back into any other public
 * member of this class.
 */
class LibCecAdapter final : public ICecAdapter {
public:
    /**
     * Configuration options for the libcec backend. Consumed once at
     * construction and immutable thereafter; runtime-mutable knobs
     * (auto-standby) live on @c CommandRouter.
     */
    struct Options {
        std::string deviceName;
        bool        autoPowerOn;
        bool        autoWakeAVR;
        bool        activateSource;
        bool        systemAudioMode;
        CEC::cec_logical_addresses wakeDevices;
        CEC::cec_logical_addresses powerOffDevices;

        Options()
            : deviceName("CEC Control"),
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

    LibCecAdapter(const LibCecAdapter&)            = delete;
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
    // libcec owns the ICECAdapter instance; ownership is released back
    // to it via CECDestroy() rather than `delete`. Using the default
    // unique_ptr deleter would invoke `delete` on a libcec-allocated
    // object, which is outside the documented API contract.
    struct AdapterDeleter {
        void operator()(CEC::ICECAdapter* adapter) const noexcept {
            if (adapter) ::CECDestroy(adapter);
        }
    };
    using AdapterPtr = std::unique_ptr<CEC::ICECAdapter, AdapterDeleter>;

    // Configuration
    Options     m_options;
    std::string m_portName;

    // libcec adapter. m_callbacks is a plain non-owning struct whose
    // address we hand to libcec via m_config.callbacks; libcec treats
    // the pointer as borrowed (consistent with AdapterDeleter's
    // CECDestroy note — libcec never free()s anything we hand it).
    // Value-initialised so every function slot starts out nullptr;
    // the constructor fills in the ones we use.
    AdapterPtr                 m_adapter;
    CEC::ICECCallbacks         m_callbacks{};
    CEC::libcec_configuration  m_config;

    // Cross-thread connection hint. Written by libcec's alert thread
    // in the CEC_ALERT_CONNECTION_LOST path and by the owning worker
    // in openConnection/closeConnection/reopenConnection. Read without
    // synchronisation by callers that want a cheap pre-flight; those
    // callers treat the value as advisory.
    std::atomic<bool> m_connected;

    // Callbacks — install-once at construction; libcec reads them
    // from its internal threads without a lock. Never reassigned.
    const std::function<void()> m_tvStandbyCallback;
    const std::function<void()> m_connectionLostCallback;

    /**
     * Detect available CEC adapter hardware. Caches the first-found
     * port in @c m_portName.
     */
    bool detectAdapter();

    // libcec callback trampolines
    static void cecLogCallback(void* cbParam, const CEC::cec_log_message* message);
    static void cecCommandCallback(void* cbParam, const CEC::cec_command* command);
    static void cecAlertCallback(void* cbParam, const CEC::libcec_alert alert,
                                 const CEC::libcec_parameter param);
};

} // namespace cec_control
