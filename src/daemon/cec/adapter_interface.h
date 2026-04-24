#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <libcec/cec.h>

namespace cec_control {

/**
 * @class ICecAdapter
 * @brief Abstract interface over the CEC adapter stack.
 *
 * Implementations wrap a concrete backend (libcec today; a kernel
 * @c /dev/cecN driver is the intended second citizen) and expose the
 * minimum surface the @c CommandDispatcher and @c AdapterLifecycle
 * need — lifecycle, commands, queries, and broadcast helpers.
 *
 * Invariants every implementation MUST uphold:
 *
 *  - @b Callback threading. Members of @ref Callbacks fire on an
 *    arbitrary backend-internal thread (libcec's command / alert
 *    threads today; unspecified for other backends). Implementations
 *    MUST be thread-safe and MUST NOT block — a stalled callback risks
 *    deadlock against a concurrent call into the adapter.
 *
 *  - @b Connection hint. @c isConnected() is advisory: the value may
 *    go stale between the caller's read and any follow-up call. Every
 *    member re-checks its own state under the implementation's own
 *    synchronisation; callers need not serialise the two.
 *
 *  - @b Reopen cost. @c reopenConnection() may be a full
 *    destroy-and-recreate cycle (as libcec requires — the library binds
 *    per-client state to the handle returned by @c CECInitialise, so a
 *    plain Close+Open leaves that state half-initialised). The call
 *    is therefore potentially seconds-long; callers MUST NOT hold an
 *    orchestration lock across it.
 *
 * @note Not to be confused with libcec's @c CEC::ICECAdapter (all-caps
 *       @c ICECAdapter, distinct namespace). The two are separated by
 *       namespace qualifier and letter case only.
 */
class ICecAdapter {
public:
    /**
     * Typed observation of a CEC bus event the daemon cares about.
     *
     * Produced by the backend after it matches either a filtered
     * opcode on the command-receive path (@c TvStandby /
     * @c TvPowerReport / @c ActiveSource) or a client-local
     * source-activation edge (@c HostActivated / @c HostDeactivated),
     * and delivered via @ref Callbacks::onObservation on a backend-
     * internal thread. The @c kind tag selects the payload field that
     * is meaningful for the event; other payload fields stay at their
     * default sentinels and MUST NOT be read.
     *
     * Trivially copyable — passes through @c std::function and
     * @c MainThreadWork closures without a heap allocation, which
     * matters because the adapter hops every observation from the
     * backend thread to the main thread via that queue.
     */
    struct Observation {
        enum class Kind {
            TvStandby,
            TvPowerReport,
            ActiveSource,
            HostActivated,
            HostDeactivated,
        };
        Kind kind{};

        /** Meaningful only when @c kind == @c Kind::ActiveSource. */
        uint16_t physicalAddress{0};

        /** Meaningful only when @c kind == @c Kind::TvPowerReport. */
        CEC::cec_power_status power{CEC::CEC_POWER_STATUS_UNKNOWN};

        /**
         * Meaningful only when @c kind is @c Kind::HostActivated or
         * @c Kind::HostDeactivated — the logical address of the
         * backend's own client whose active-source state just
         * changed. @c CECDEVICE_UNKNOWN on every other kind.
         */
        CEC::cec_logical_address logical{CEC::CECDEVICE_UNKNOWN};
    };

    /**
     * Outbound hooks the adapter fires from backend-internal threads.
     * Installed once at construction and never re-seated; any late
     * re-wiring would race readers on the backend's thread. Either
     * member may be empty — the adapter no-ops the corresponding event.
     */
    struct Callbacks {
        /**
         * Backend-observed CEC bus event; see @ref Observation. Fires
         * on the backend's command-receive thread (libcec today).
         */
        std::function<void(Observation)> onObservation;
        /** Backend lost contact with the adapter hardware. */
        std::function<void()> onConnectionLost;
    };

    virtual ~ICecAdapter() = default;

    ICecAdapter() = default;
    ICecAdapter(const ICecAdapter&) = delete;
    ICecAdapter& operator=(const ICecAdapter&) = delete;
    ICecAdapter(ICecAdapter&&) = delete;
    ICecAdapter& operator=(ICecAdapter&&) = delete;

    // Lifecycle ---------------------------------------------------------

    /**
     * Load and prepare the backend. A failed @c initialize() leaves
     * the adapter in a state where no other member may be called; the
     * caller must discard the instance. Must be invoked before
     * @c openConnection().
     */
    [[nodiscard]] virtual bool initialize() = 0;

    /** Open the adapter connection. Prerequisite: @c initialize(). */
    [[nodiscard]] virtual bool openConnection() = 0;

    /** Close the connection. Idempotent; safe to call when already closed. */
    virtual void closeConnection() = 0;

    /**
     * Tear down and re-establish the connection. See @em reopen-cost
     * at class scope.
     */
    [[nodiscard]] virtual bool reopenConnection() = 0;

    /** Connection hint — see @em connection-hint at class scope. */
    [[nodiscard]] virtual bool isConnected() const = 0;

    // Commands ----------------------------------------------------------
    //
    // libcec's enum types remain in the signature for this phase.
    // Aliasing them into the @c cec_control namespace is a later
    // cleanup and does not block this interface.

    [[nodiscard]] virtual bool powerOnDevice(CEC::cec_logical_address address) = 0;
    [[nodiscard]] virtual bool standbyDevice(CEC::cec_logical_address address) = 0;
    [[nodiscard]] virtual bool volumeUp() = 0;
    [[nodiscard]] virtual bool volumeDown() = 0;
    [[nodiscard]] virtual bool toggleMute() = 0;
    [[nodiscard]] virtual bool sendKeypress(CEC::cec_logical_address address,
                                            CEC::cec_user_control_code key,
                                            bool release) = 0;
    [[nodiscard]] virtual bool setStreamPath(uint16_t physicalAddress) = 0;

    // Queries -----------------------------------------------------------

    [[nodiscard]] virtual uint16_t getDevicePhysicalAddress(
        CEC::cec_logical_address address) const = 0;
    [[nodiscard]] virtual bool isDeviceActive(
        CEC::cec_logical_address address) const = 0;
    [[nodiscard]] virtual CEC::cec_power_status getDevicePowerStatus(
        CEC::cec_logical_address address) const = 0;
    [[nodiscard]] virtual std::string getDeviceOSDName(
        CEC::cec_logical_address address) const = 0;
    [[nodiscard]] virtual CEC::cec_logical_addresses getActiveDevices() const = 0;
    [[nodiscard]] virtual CEC::cec_logical_address getActiveSource() const = 0;

    // Broadcast-style helpers ------------------------------------------
    //
    // Default argument is resolved statically, but every daemon-side
    // call site goes through an @c ICecAdapter reference or pointer,
    // so the interface's default is the one that applies.

    [[nodiscard]] virtual bool standbyDevices(
        CEC::cec_logical_address address = CEC::CECDEVICE_BROADCAST) = 0;
    [[nodiscard]] virtual bool powerOnDevices(
        CEC::cec_logical_address address = CEC::CECDEVICE_BROADCAST) = 0;
};

} // namespace cec_control
