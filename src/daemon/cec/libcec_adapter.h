#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include <libcec/cec.h>

#include "../../common/logger.h"
#include "adapter_config.h"
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
     * Construct a libcec-backed adapter. Callbacks are install-once
     * (see @c ICecAdapter::Callbacks).
     */
    LibCecAdapter(AdapterConfig config, Callbacks callbacks);
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

    // ---------------------------------------------------------------
    // Member declaration order is a correctness invariant.
    //
    // libcec's internal command and alert threads, spawned by
    // ICECAdapter::Open(), read:
    //   - m_callbacks      (via the pointer stored in m_libcecConfig)
    //   - m_libcecConfig   (libcec keeps its own pointer to the
    //                       configuration we hand to CECInitialise)
    //   - m_connected                 (via cbParam → this → &field)
    //   - m_observationCallback       (ditto)
    //   - m_connectionLostCallback    (ditto)
    //
    // Those threads are joined by ICECAdapter::Close(), which runs as
    // part of the AdapterDeleter — i.e. inside m_adapter's destructor.
    // Declaring m_adapter last means it is destroyed FIRST, joining
    // every libcec thread before any of the fields above are torn
    // down. ~LibCecAdapter() also calls closeConnection() explicitly
    // as a first line of defence; member declaration order is the
    // structural invariant that survives a refactor of the destructor
    // body.
    // ---------------------------------------------------------------

    // Configuration, read-only after construction.
    AdapterConfig m_config;
    std::string   m_portName;

    // libcec's non-owning view of our callback struct. m_callbacks is
    // value-initialised so every function slot starts out nullptr; the
    // constructor fills in the ones we use and hands the address to
    // libcec via m_libcecConfig.callbacks. libcec treats the pointer
    // as borrowed (consistent with AdapterDeleter's CECDestroy note —
    // libcec never free()s anything we hand it).
    CEC::ICECCallbacks         m_callbacks{};
    CEC::libcec_configuration  m_libcecConfig;

    // Cross-thread connection hint. Written by libcec's alert thread
    // in the CEC_ALERT_CONNECTION_LOST path and by the owning worker
    // in openConnection/closeConnection/reopenConnection. Read without
    // synchronisation by callers that want a cheap pre-flight; those
    // callers treat the value as advisory.
    std::atomic<bool> m_connected;

    // Install-once at construction; libcec reads them from its internal
    // threads without a lock. Never reassigned.
    const std::function<void(Observation)> m_observationCallback;
    const std::function<void()>            m_connectionLostCallback;

    // libcec adapter handle. MUST stay last — see the block comment
    // above. The deleter calls CECDestroy(), which joins libcec's
    // internal threads before control returns.
    AdapterPtr m_adapter;

    /**
     * Detect available CEC adapter hardware. Caches the first-found
     * port in @c m_portName.
     */
    bool detectAdapter();

    /**
     * Invoke @p fn iff the adapter is initialised and the
     * connection hint reports connected; otherwise return
     * @p fallback unchanged. Centralises the identical two-line
     * pre-flight that otherwise fronts every public command and
     * query in this class.
     *
     * Const-qualified so both const and non-const members can
     * invoke it. @c std::unique_ptr 's non-propagating const on the
     * pointee (@c operator->() returns @c T* regardless) lets the
     * lambda call non-const libcec methods even from a const
     * context — the pattern libcec itself encourages.
     */
    template <typename R, typename Fn>
    R callIfConnected(R fallback, Fn&& fn) const {
        if (!m_adapter || !m_connected.load(std::memory_order_acquire))
            return fallback;
        return fn();
    }

    // libcec callback trampolines
    static void cecLogCallback(void* cbParam, const CEC::cec_log_message* message);
    static void cecCommandCallback(void* cbParam, const CEC::cec_command* command);
    static void cecAlertCallback(void* cbParam, const CEC::libcec_alert alert,
                                 const CEC::libcec_parameter param);
};

} // namespace cec_control
