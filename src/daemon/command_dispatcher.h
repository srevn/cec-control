#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "../common/messages.h"
#include "app_config.h"
#include "command_throttler.h"

namespace cec_control {

class AdapterLifecycle;
class AdapterWorker;
class ICecAdapter;
class MainThreadWork;

/**
 * @class CommandDispatcher
 * @brief Translates wire commands into adapter calls and owns the
 *        auto-standby policy (wire-side flag flip + libcec-side TV
 *        standby observation).
 *
 * Single-threaded for wire dispatch: every call to @c dispatch happens
 * on the main event loop. Adapter-driven commands hand the response
 * sink to a worker job that posts the invocation back through
 * @c MainThreadWork. The class also exposes a public @c replay path
 * used by @c PowerSupervisor after a successful resume — see
 * @c AdapterLifecycle's class doc for the data flow.
 *
 * ## Dispatch paths
 *
 * @c dispatch distinguishes three classes of command, each replying to
 * its @c ResponseSink from a different point:
 *
 *  - @b Gate-only (shutdown rejection, suspended non-queueable) —
 *    replies synchronously on the main thread.
 *  - @b State-only (suspend queue push, @c CMD_AUTO_STANDBY flag flip)
 *    — replies synchronously after a minimal state mutation.
 *  - @b Adapter-driven (volume, power, source, mute,
 *    @c CMD_RESTART_ADAPTER) — submits a worker job; the job invokes
 *    the sink via @c MainThreadWork::post on completion, so the client
 *    sees the genuine outcome rather than a fire-and-forget ack.
 *
 * ## Auto-standby
 *
 * The dispatcher owns both sides of auto-standby: @c CMD_AUTO_STANDBY
 * toggles @c m_autoStandbyEnabled (the wire side), and @c onTvStandby
 * reads the flag from libcec's command thread, firing the install-once
 * @c onSuspendRequested callback when enabled (the adapter side).
 * The flag is atomic because @c onTvStandby is called off the main
 * thread; the callback itself is set at construction and never
 * rewired, so calling it without a lock is safe.
 *
 * ## Shutdown
 *
 * The shutdown gate is purely defensive: by the time
 * @c CECDaemon::stop calls @c shutdown here, the socket server is
 * already torn down, so no path can call @c dispatch afterwards. The
 * gate is retained because the cost is one bool comparison and it
 * documents the contract for future entry points.
 */
class CommandDispatcher {
public:
    /**
     * Outbound hook fired by the dispatcher. Supplied at construction
     * and never rewired. Invoked on the thread that observed the
     * trigger — for @c onSuspendRequested that is libcec's command
     * thread. The installed closure MUST be thread-safe and
     * non-blocking; in practice a single @c MainThreadWork::post.
     */
    struct Callbacks {
        /** TV-standby plus auto-standby policy agreed the system should sleep. */
        std::function<void()> onSuspendRequested;
    };

    /**
     * @param config      Read-only snapshot; the dispatcher extracts
     *                    its seed values (throttler tuning, initial
     *                    policy flags) at construction and does not
     *                    retain a reference.
     * @param worker      Non-owning; must outlive @c this. Every
     *                    adapter call is submitted here.
     * @param work        Non-owning; must outlive @c this. Used to hop
     *                    worker-side completions back to the main
     *                    thread.
     * @param lifecycle   Non-owning; must outlive @c this. Read for
     *                    @c isSuspended on the gate paths and written
     *                    via @c enqueue on the suspended-inline path.
     * @param callbacks   Install-once outbound hooks; see @c Callbacks.
     */
    CommandDispatcher(const AppConfig&  config,
                      AdapterWorker&    worker,
                      MainThreadWork&   work,
                      AdapterLifecycle& lifecycle,
                      Callbacks         callbacks);

    ~CommandDispatcher() = default;

    CommandDispatcher(const CommandDispatcher&)            = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;

    /** Mark the dispatcher as shut down. Idempotent. Main thread only. */
    void shutdown();

    /** @c true once @c shutdown has been called. */
    [[nodiscard]] bool isShutdown() const noexcept;

    /**
     * Route an incoming wire command. See the class-level doc-comment
     * for the three dispatch classes and where the sink fires. Main
     * thread only.
     */
    void dispatch(Message command, ResponseSink reply);

    /**
     * Re-issue @p commands as fresh worker jobs through the same
     * @c executeOnAdapter + throttler path used by live dispatch. Main
     * thread only. Called by @c PowerSupervisor with the vector
     * drained by @c AdapterLifecycle::resumeAsync after a successful
     * reopen. No-op for an empty input.
     */
    void replay(std::vector<Message> commands);

    /**
     * Invoked by the daemon's adapter forwarder when libcec reports a
     * TV standby command. Runs on libcec's command thread; must be
     * thread-safe and non-blocking. Reads the auto-standby atomic and,
     * if enabled, fires the install-once @c onSuspendRequested
     * callback.
     */
    void onTvStandby();

private:
    /**
     * Apply @c CMD_AUTO_STANDBY's flag flip. Main thread only; no
     * adapter touch, so no worker hop.
     */
    Message applyAutoStandbyInline(const Message& command);

    /**
     * Policy for a command arriving while suspended: queue it for
     * post-resume replay or reject. Main thread only.
     */
    Message handleSuspendedInline(const Message& command);

    /**
     * Phase-2 body: runs on the worker thread against the supplied
     * adapter reference. Shared between live dispatch and @c replay.
     */
    Message executeOnAdapter(ICecAdapter& adapter, const Message& command);

    AdapterWorker&    m_worker;
    MainThreadWork&   m_work;
    AdapterLifecycle& m_lifecycle;
    CommandThrottler  m_throttler;

    // Shutdown gate. Main-thread only — see the class-level doc comment.
    bool m_shutdownComplete = false;

    // Queue-during-suspend policy. Main-thread-only reader (dispatch
    // is main-thread), main-thread-only writer (only a future SIGHUP
    // handler would flip it, also main-thread). A plain bool is
    // enough; promote to atomic only if a cross-thread reader appears.
    bool m_queueCommandsDuringSuspend;

    // Auto-standby policy toggle. Atomic because the reader is libcec's
    // command thread via onTvStandby.
    std::atomic<bool> m_autoStandbyEnabled;

    // Install-once outbound hook. Invoked inline from onTvStandby on
    // libcec's command thread.
    const std::function<void()> m_suspendCallback;
};

} // namespace cec_control
