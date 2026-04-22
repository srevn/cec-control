#pragma once

#include <atomic>
#include <chrono>
#include <functional>

#include "../common/messages.h"
#include "app_config.h"
#include "command_throttler.h"
#include "power/suspend_queue.h"

namespace cec_control {

class AdapterWorker;
class ICecAdapter;
class MainThreadWork;

/**
 * @class CommandRouter
 * @brief Orchestrates CEC wire-command dispatch and the suspend /
 *        resume lifecycle around a single @c AdapterWorker.
 *
 * The router is single-threaded: every public member is called from
 * the main event loop. Adapter-driven operations are handed to the
 * @c AdapterWorker via @c submit; their completions hop back to the
 * main thread through @c MainThreadWork::post from inside each worker
 * job. There is no router-side lock — serialisation against libcec is
 * provided by the worker thread, and the router's own state is visited
 * only from the main thread.
 *
 * ## Dispatch paths
 *
 * @c dispatch distinguishes three classes of command, each replying to
 * its @c ResponseSink from a different point:
 *
 *  - @b Gate-only (shutdown rejection, suspended non-queueable) —
 *    replies synchronously on the main thread.
 *  - @b State-only (suspend queue push, @c CMD_AUTO_STANDBY flag flip,
 *    @c CMD_RESTART_ADAPTER fire-and-forget reopen) — replies
 *    synchronously after a minimal state mutation.
 *  - @b Adapter-driven (volume, power, source, mute) — submits a
 *    worker job; the job invokes the sink via @c MainThreadWork::post
 *    on completion.
 *
 * ## Suspend / resume coordination
 *
 * @c suspendAsync, @c resumeAsync, and @c reconnectAsync split into a
 * main-thread phase-1 (flag flips, state checks) and a worker-thread
 * phase-2 (libcec calls). Each posts its continuation through
 * @c MainThreadWork so the @c onDone callback fires on the main
 * thread. A resume drains the suspend queue and submits each queued
 * command as a fresh worker job before firing @c onDone — the caller
 * observes "reopened" rather than "reopened + replayed". Newly
 * arriving dispatches interleave with the replays in FIFO order on
 * the worker queue; at the CEC layer this is indistinguishable from
 * the pre-refactor pool-worker model.
 */
class CommandRouter {
public:
    /**
     * Outbound hook fired by the router. Supplied at construction and
     * never rewired. Invoked on the thread that observed the trigger —
     * for @c onSuspendRequested that is libcec's command thread. The
     * installed closure MUST be thread-safe and non-blocking; in
     * practice a single @c MainThreadWork::post.
     */
    struct Callbacks {
        /** TV-standby plus auto-standby policy agreed the system should sleep. */
        std::function<void()> onSuspendRequested;
    };

    /**
     * Response delivery target for @c dispatch. The router invokes it
     * at most once per @c dispatch call, either synchronously on the
     * main thread (gate-only / state-only paths) or asynchronously via
     * @c MainThreadWork::post (adapter-driven path).
     */
    using ResponseSink = std::function<void(Message)>;

    /**
     * @param config      Read-only snapshot; the router extracts its
     *                    seed values (throttler tuning, initial policy
     *                    flags) at construction and does not retain a
     *                    reference.
     * @param worker      Non-owning; must outlive @c this. Every
     *                    adapter call is submitted here.
     * @param work        Non-owning; must outlive @c this. Used to hop
     *                    worker-side completions back to the main
     *                    thread.
     * @param callbacks   Install-once outbound hooks; see @c Callbacks.
     */
    CommandRouter(const AppConfig& config,
                  AdapterWorker&   worker,
                  MainThreadWork&  work,
                  Callbacks        callbacks);

    ~CommandRouter() = default;

    CommandRouter(const CommandRouter&)            = delete;
    CommandRouter& operator=(const CommandRouter&) = delete;

    /**
     * Mark the router as shut down and discard any commands still
     * parked in the suspend queue. Main thread only. Idempotent. Does
     * not touch the adapter — the worker's own @c stop() sequence
     * closes it on the worker thread.
     */
    void shutdown();

    /**
     * Route an incoming wire command. See the class-level doc-comment
     * for the three dispatch classes and where the sink fires. Main
     * thread only.
     */
    void dispatch(Message command, ResponseSink reply);

    /**
     * Enter the suspended state and run pre-sleep CEC actions on the
     * worker. Main thread only. @p onDone fires on the main thread
     * with the elapsed worker-side duration; on a shutdown or
     * already-suspended state the callback still fires (with a zero
     * duration) so the caller's lifecycle FSM keeps progressing.
     */
    void suspendAsync(std::function<void(std::chrono::milliseconds)> onDone);

    /**
     * Reopen the adapter on the worker, drain the suspend queue on
     * main, and submit each queued command as a fresh worker job.
     * Main thread only. @p onDone fires on the main thread with the
     * post-reopen adapter state; a @c false value tells the caller's
     * lifecycle FSM to arm the post-resume retry timer.
     */
    void resumeAsync(std::function<void(bool)> onDone);

    /**
     * Reopen the adapter on the worker iff not shutdown and not
     * suspended. Main thread only. @p onDone fires on the main thread
     * with the reopen outcome; @c false covers both the gated cases
     * (shutdown / suspended) and a genuine reopen failure.
     */
    void reconnectAsync(std::function<void(bool)> onDone);

    /** @c true iff the router is currently in the suspended state. */
    [[nodiscard]] bool isSuspended() const noexcept;

    /**
     * Invoked by the daemon's adapter forwarder when libcec reports a
     * TV standby command. Runs on libcec's command thread; must be
     * thread-safe and non-blocking. Reads the auto-standby atomic and,
     * if enabled, fires @c m_suspendCallback.
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
     * adapter reference. Shared between live dispatch and post-resume
     * replay.
     */
    Message executeOnAdapter(ICecAdapter& adapter, const Message& command);

    /**
     * Main-thread continuation fired after the resume worker finishes
     * @c reopenConnection + @c powerOnDevices. Drains the suspend
     * queue, exits the suspended state, submits replays, and invokes
     * the caller's @p onDone.
     */
    void onResumeWorkerComplete(bool adapterValid,
                                std::function<void(bool)> onDone);

    AdapterWorker&   m_worker;
    MainThreadWork&  m_work;
    CommandThrottler m_throttler;

    // Shutdown gate. Main-thread only — see the class-level doc comment
    // for the single-threaded access model.
    bool m_shutdownComplete = false;

    // Queue-during-suspend policy. Main-thread-only reader (dispatch
    // is main-thread), main-thread-only writer (only a future SIGHUP
    // handler would flip it, also main-thread). A plain bool is
    // enough; promote to atomic only if a cross-thread reader appears.
    bool m_queueCommandsDuringSuspend;

    // Suspend flag + queued commands. Main-thread only.
    SuspendQueue m_suspendQueue;

    // Auto-standby policy toggle. Atomic because the reader is libcec's
    // command thread via onTvStandby.
    std::atomic<bool> m_autoStandbyEnabled;

    // Install-once outbound hook. Invoked inline from onTvStandby on
    // libcec's command thread.
    const std::function<void()> m_suspendCallback;
};

} // namespace cec_control
