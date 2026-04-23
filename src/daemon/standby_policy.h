#pragma once

#include <functional>

#include "../common/messages.h"
#include "cec/adapter_interface.h"

namespace cec_control {

/**
 * @class StandbyPolicy
 * @brief "Auto-suspend on TV standby" policy: wire-toggled flag plus
 *        the main-thread observation that triggers the suspend.
 *
 * Extracted from @c CommandDispatcher. The dispatcher routes the
 * @c CMD_AUTO_STANDBY wire command to @c apply, and the daemon's
 * adapter-observation forwarder routes @c TvStandby observations to
 * @c observe — keeping everything auto-standby owns in one place.
 *
 * ## Threading
 *
 * Both entry points run on the main thread:
 *
 *  - @c apply is invoked from @c CommandDispatcher::dispatch on the
 *    event loop.
 *  - @c observe is invoked from the daemon's adapter-observation
 *    forwarder, which hops every libcec observation through
 *    @c MainThreadWork before dispatching. The policy therefore never
 *    runs on a libcec thread; readers and writers are single-threaded,
 *    so the flag is a plain @c bool and the install-once suspend-
 *    request callback fires inline on the main thread (typically a
 *    @c MainThreadWork::post to make an sd-bus call).
 *
 * ## Ownership
 *
 * Held by @c unique_ptr on @c CECDaemon, constructed after the
 * adapter worker (so libcec callbacks arriving during the
 * construction window are absorbed by the daemon forwarder's null
 * guard) and destroyed after the worker has been joined. The
 * dispatcher holds a non-owning reference.
 */
class StandbyPolicy {
public:
    /**
     * @param initialEnabled      Seed for the flag; subsequent
     *                            @c CMD_AUTO_STANDBY commands can
     *                            toggle it at runtime.
     * @param onSuspendRequested  Install-once hook fired inline from
     *                            @c observe on the main thread when a
     *                            @c TvStandby observation arrives and
     *                            the flag is set. In practice a single
     *                            @c MainThreadWork::post that reaches
     *                            sd-bus on the event-loop thread.
     */
    StandbyPolicy(bool initialEnabled,
                  std::function<void()> onSuspendRequested);

    StandbyPolicy(const StandbyPolicy&)            = delete;
    StandbyPolicy& operator=(const StandbyPolicy&) = delete;
    StandbyPolicy(StandbyPolicy&&)                 = delete;
    StandbyPolicy& operator=(StandbyPolicy&&)      = delete;

    /**
     * Apply a @c CMD_AUTO_STANDBY wire command: read @p command.data[0]
     * as the new enabled value (non-zero = @c true). Returns
     * @c RESP_SUCCESS on a well-formed payload, @c RESP_ERROR on an
     * empty payload. Main thread only.
     */
    [[nodiscard]] Message apply(const Message& command);

    /**
     * Consume a CEC bus observation forwarded by the daemon. Filters
     * for @c Observation::Kind::TvStandby; other kinds are ignored.
     * If the policy is enabled, fires the install-once suspend-request
     * callback inline. Main thread only.
     */
    void observe(const ICecAdapter::Observation& obs);

    /** Flag snapshot. Main thread only. */
    [[nodiscard]] bool isEnabled() const noexcept;

private:
    bool                        m_enabled;
    const std::function<void()> m_suspendCallback;
};

} // namespace cec_control
