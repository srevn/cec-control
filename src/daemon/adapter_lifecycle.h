#pragma once

#include <chrono>
#include <functional>
#include <vector>

#include "../common/messages.h"
#include "power/suspend_queue.h"

namespace cec_control {

class AdapterWorker;
class MainThreadWork;

/**
 * @class AdapterLifecycle
 * @brief Owns the suspend / resume / reconnect orchestration over a
 *        single @c AdapterWorker, plus the @c SuspendQueue holding
 *        commands parked between @c suspendAsync and the matching
 *        @c resumeAsync completion.
 *
 * Single-threaded: every public member is called from the main event
 * loop. Adapter-driven phases are submitted to the @c AdapterWorker;
 * each worker job posts its completion back through @c MainThreadWork
 * so the @p onDone callbacks fire on the main thread. There is no
 * lifecycle-side lock — serialisation against libcec is provided by
 * the worker thread, and this class's own state is visited only from
 * the main thread.
 *
 * ## Boundary vs. @c CommandDispatcher
 *
 * The dispatcher checks @c isSuspended before routing wire commands
 * and pushes queueable commands via @c enqueue. After a successful
 * resume, this class drains the queue and hands the drained vector
 * back through the @c resumeAsync callback alongside the adapter
 * validity; the orchestrator (@c PowerSupervisor) then re-issues the
 * commands by calling @c CommandDispatcher::replay. There is no
 * outbound reference from the lifecycle to the dispatcher; the
 * orchestrator is the bridge.
 *
 * ## Shutdown
 *
 * The shutdown gate is purely defensive: by the time
 * @c CECDaemon::stop calls @c shutdown here, the socket server and
 * D-Bus monitor have been torn down, so no path can call into the
 * other public methods. The gate is retained because a single bool
 * comparison documents the contract for future entry points and
 * matches the dispatcher's shape.
 */
class AdapterLifecycle {
public:
    /**
     * Resume completion callback. Fires on the main thread with
     * @p adapterValid (the post-reopen connection hint) and the
     * commands drained from the suspend queue. On adapter failure the
     * lifecycle discards the drained vector internally (logging the
     * count) and passes back an empty vector — callers never see
     * commands the adapter cannot deliver on.
     */
    using ResumeCallback =
        std::function<void(bool adapterValid, std::vector<Message> queued)>;

    AdapterLifecycle(AdapterWorker& worker, MainThreadWork& work) noexcept;

    ~AdapterLifecycle() = default;

    AdapterLifecycle(const AdapterLifecycle&)            = delete;
    AdapterLifecycle& operator=(const AdapterLifecycle&) = delete;

    /**
     * Mark the lifecycle as shut down and discard any commands still
     * parked in the suspend queue. Idempotent. Main thread only. Does
     * not touch the adapter — the worker's own @c stop() sequence
     * closes it on the worker thread.
     */
    void shutdown();

    /** @c true once @c shutdown has been called. */
    [[nodiscard]] bool isShutdown() const noexcept;

    /** @c true iff currently between @c suspendAsync and the matching resume. */
    [[nodiscard]] bool isSuspended() const noexcept;

    /**
     * Append @p cmd to the suspend queue iff currently suspended;
     * otherwise no-op. Mirrors @c SuspendQueue::push semantics — safe
     * to call without a prior @c isSuspended check, and the dispatcher
     * relies on this for the suspended-inline path. Main thread only.
     */
    void enqueue(const Message& cmd);

    /**
     * Enter the suspended state and run pre-sleep CEC actions on the
     * worker. Main thread only. @p onDone fires on the main thread
     * with the elapsed worker-side duration; on a shutdown or
     * already-suspended state the callback still fires (with a zero
     * duration) so the caller's lifecycle FSM keeps progressing.
     */
    void suspendAsync(std::function<void(std::chrono::milliseconds)> onDone);

    /**
     * Reopen the adapter on the worker. After the reopen completes,
     * drain the suspend queue, exit the suspended state, and invoke
     * @p onDone on the main thread with @c (adapterValid, queued).
     * On adapter failure the drained commands are discarded internally
     * with a warning log; @p onDone still fires (with an empty
     * vector) so the caller's lifecycle FSM keeps progressing.
     */
    void resumeAsync(ResumeCallback onDone);

    /**
     * Reopen the adapter on the worker iff not shutdown and not
     * suspended. Main thread only. @p onDone fires on the main thread
     * with the reopen outcome; @c false covers both the gated cases
     * (shutdown / suspended) and a genuine reopen failure.
     */
    void reconnectAsync(std::function<void(bool)> onDone);

private:
    /**
     * Main-thread continuation fired after the resume worker finishes
     * @c reopenConnection + @c powerOnDevices. Drains the suspend
     * queue, exits the suspended state, and either discards the drain
     * (adapter invalid) or hands it back to the caller via @p onDone.
     */
    void onResumeWorkerComplete(bool adapterValid, ResumeCallback onDone);

    AdapterWorker&  m_worker;
    MainThreadWork& m_work;

    // Suspend flag + queued commands. Main-thread only.
    SuspendQueue m_suspendQueue;

    // Shutdown gate. Main-thread only — see the class-level doc comment.
    bool m_shutdownComplete = false;
};

} // namespace cec_control
