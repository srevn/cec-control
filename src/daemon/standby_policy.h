#pragma once

#include <atomic>
#include <functional>

#include "../common/messages.h"

namespace cec_control {

/**
 * @class StandbyPolicy
 * @brief "Auto-suspend on TV standby" policy: wire-toggled flag plus
 *        the libcec-side observation that triggers the suspend.
 *
 * Extracted from @c CommandDispatcher. The dispatcher now routes the
 * @c CMD_AUTO_STANDBY wire command to @c apply, and the daemon's
 * adapter forwarder routes libcec's TV-standby callback to
 * @c onTvStandby — keeping everything auto-standby owns in one place.
 *
 * ## Threading
 *
 * Two entry points on two threads:
 *
 *  - @c apply runs on the main thread (dispatch is main-thread) and
 *    writes the flag with release semantics.
 *  - @c onTvStandby runs on libcec's command thread. It reads the
 *    flag with acquire semantics and, if enabled, fires the install-
 *    once suspend-request callback verbatim. The callback is
 *    typically a @c MainThreadWork::post that re-enters the main
 *    thread to make an sd-bus call.
 *
 * The flag is atomic because the reader is off-main; the callback is
 * assigned at construction and never rewired, so invoking it without
 * a lock is safe.
 *
 * ## Ownership
 *
 * Held by @c unique_ptr on @c CECDaemon, constructed after the
 * adapter worker (so libcec callbacks arriving during the
 * construction window are absorbed by the daemon forwarder's null
 * guard) and destroyed after the worker has been joined (so no
 * libcec thread can still fire into a destroyed policy). The
 * dispatcher holds a non-owning reference.
 */
class StandbyPolicy {
public:
    /**
     * @param initialEnabled      Seed for the flag; subsequent
     *                            @c CMD_AUTO_STANDBY commands can
     *                            toggle it at runtime.
     * @param onSuspendRequested  Install-once hook fired inline from
     *                            @c onTvStandby on libcec's command
     *                            thread when the flag is set. Must be
     *                            thread-safe and non-blocking; in
     *                            practice a single
     *                            @c MainThreadWork::post.
     */
    StandbyPolicy(bool initialEnabled,
                  std::function<void()> onSuspendRequested);

    StandbyPolicy(const StandbyPolicy&)            = delete;
    StandbyPolicy& operator=(const StandbyPolicy&) = delete;
    StandbyPolicy(StandbyPolicy&&)                 = delete;
    StandbyPolicy& operator=(StandbyPolicy&&)      = delete;

    /**
     * Apply a @c CMD_AUTO_STANDBY wire command: read @p command.data[0]
     * as the new enabled value (non-zero = @c true) and store with
     * release semantics. Returns @c RESP_SUCCESS on a well-formed
     * payload, @c RESP_ERROR on an empty payload. Main thread only.
     */
    [[nodiscard]] Message apply(const Message& command);

    /**
     * Adapter-forwarder entry point: libcec reports a TV standby
     * command. Runs on libcec's command thread. Acquires the flag;
     * if disabled, logs at @c DEBUG and returns. If enabled, fires
     * the install-once suspend-request callback.
     */
    void onTvStandby();

    /** Flag snapshot (acquire). Valid from any thread. */
    [[nodiscard]] bool isEnabled() const noexcept;

private:
    std::atomic<bool>           m_enabled;
    const std::function<void()> m_suspendCallback;
};

} // namespace cec_control
