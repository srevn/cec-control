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
 *    so the flags are plain @c bool and the install-once suspend-
 *    request callback fires inline on the main thread (typically a
 *    @c MainThreadWork::post to make an sd-bus call).
 *
 * ## Arming gate
 *
 * @c observe short-circuits until @c arm() has been called. The
 * daemon posts @c arm() onto @c MainThreadWork at the end of
 * @c start(); any @c TvStandby observation libcec emitted between
 * @c Open() returning and the loop's first drain therefore runs
 * *before* the arming post in FIFO order, sees @c !m_armed, and is
 * absorbed. Without this gate a @c STANDBY opcode that happened to
 * arrive during the narrow pre-drain window would trigger a system
 * suspend the moment the loop started — the blast radius of
 * "your machine suspended immediately after boot". The policy is
 * already edge-triggered (@c STANDBY is never emitted for historical
 * state), so dropping a pre-arm edge is no worse than missing the
 * same edge during any period the daemon was not running.
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
     * If the policy is armed and enabled, fires the install-once
     * suspend-request callback inline. Observations arriving before
     * @c arm() are absorbed — see the "Arming gate" section at class
     * scope. Main thread only.
     */
    void observe(const ICecAdapter::Observation& obs);

    /**
     * Flip the armed flag. Until @c arm() runs, @c observe drops every
     * @c TvStandby it sees; after it runs, observations trigger the
     * configured suspend callback subject to the usual enabled check.
     * Idempotent — subsequent calls are no-ops. Main thread only; the
     * daemon posts this onto @c MainThreadWork at the end of
     * @c start() so that any observations already queued at that
     * point drain (and are absorbed) ahead of it in FIFO order.
     */
    void arm();

    /** Flag snapshot. Main thread only. */
    [[nodiscard]] bool isEnabled() const noexcept;

private:
    bool                        m_enabled;
    bool                        m_armed = false;
    const std::function<void()> m_suspendCallback;
};

} // namespace cec_control
