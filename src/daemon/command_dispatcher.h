#pragma once

#include <vector>

#include "../common/messages.h"
#include "app_config.h"
#include "command_throttler.h"

namespace cec_control {

class AdapterLifecycle;
class AdapterWorker;
class ICecAdapter;
class MainThreadWork;
class StandbyPolicy;
struct DispatchSpec;

/**
 * @class CommandDispatcher
 * @brief Translates wire commands into adapter calls.
 *
 * Single-threaded for wire dispatch: every call to @c dispatch happens
 * on the main event loop. @c DispatchClass::AdapterCall commands hand
 * the response sink to a worker job that posts the invocation back
 * through @c MainThreadWork. The class also exposes a public
 * @c replay path used by @c PowerSupervisor after a successful resume
 * — see @c AdapterLifecycle's class doc for the data flow.
 *
 * ## Dispatch paths
 *
 * Every incoming command is classified via @c findDispatchByType in
 * @c command_dispatch.h. The dispatcher observes three outcomes:
 *
 *  - @b Gated reject (shutdown gate tripped, unknown type, suspended
 *    + non-queueable) — replies @c RESP_ERROR synchronously on the
 *    main thread.
 *  - @b DispatchClass::StateOnly — delegated to @c StandbyPolicy::apply
 *    and replied synchronously. @c CMD_AUTO_STANDBY is the only
 *    @c StateOnly row in @c kDispatchTable today.
 *  - @b DispatchClass::AdapterCall (volume, power, source, mute,
 *    @c CMD_RESTART_ADAPTER) — @c submitAdapterWork submits a worker
 *    job; the job invokes the sink via @c MainThreadWork::post on
 *    completion, so the client sees the genuine outcome rather than a
 *    fire-and-forget ack.
 *
 * @c DispatchClass::SupervisorIntercepted rows never reach this class:
 * @c CECDaemon::handleCommand short-circuits @c CMD_SUSPEND and
 * @c CMD_RESUME straight into @c PowerSupervisor. The dispatcher's
 * own switch rejects any stray one defensively; the startup-time
 * @c validateDispatchTable invariant makes the stray case unreachable
 * in practice.
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
     * @param config        Read-only snapshot; the dispatcher extracts
     *                      its seed values (throttler tuning, queue-
     *                      during-suspend flag) at construction and
     *                      does not retain a reference.
     * @param worker        Non-owning; must outlive @c this. Every
     *                      adapter call is submitted here.
     * @param work          Non-owning; must outlive @c this. Used to
     *                      hop worker-side completions back to the
     *                      main thread.
     * @param lifecycle     Non-owning; must outlive @c this. Read for
     *                      @c isSuspended on the gate paths and
     *                      written via @c enqueue on the suspended-
     *                      inline path.
     * @param standbyPolicy Non-owning; must outlive @c this. Handles
     *                      @c DispatchClass::StateOnly commands.
     */
    CommandDispatcher(const AppConfig&  config,
                      AdapterWorker&    worker,
                      MainThreadWork&   work,
                      AdapterLifecycle& lifecycle,
                      StandbyPolicy&    standbyPolicy);

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

private:
    /**
     * Policy for a command arriving while suspended: queue it for
     * post-resume replay or reject. Main thread only. The caller has
     * already located the dispatch spec; pass it by reference to save
     * a second linear lookup.
     */
    Message handleSuspendedInline(const Message& command,
                                   const DispatchSpec& spec);

    /**
     * Submit @p command to the worker for @c DispatchClass::AdapterCall
     * dispatch; post the resulting @c Message back to the main thread
     * via @c MainThreadWork. Main thread only.
     */
    void submitAdapterWork(const DispatchSpec& spec,
                           Message command,
                           ResponseSink reply);

    /**
     * Worker-thread body: invokes @c spec.adapterHandler under the
     * @c isConnected gate dictated by @c spec.requiresAdapterConnection,
     * catches exceptions, and returns @c RESP_SUCCESS / @c RESP_ERROR.
     * Shared between live dispatch (@c submitAdapterWork) and
     * @c replay.
     */
    Message executeOnAdapter(ICecAdapter& adapter,
                             const Message& command,
                             const DispatchSpec& spec);

    AdapterWorker&    m_worker;
    MainThreadWork&   m_work;
    AdapterLifecycle& m_lifecycle;
    StandbyPolicy&    m_standbyPolicy;
    CommandThrottler  m_throttler;

    // Shutdown gate. Main-thread only — see the class-level doc comment.
    bool m_shutdownComplete = false;

    // Queue-during-suspend policy. Main-thread-only reader (dispatch
    // is main-thread), main-thread-only writer (only a future SIGHUP
    // handler would flip it, also main-thread). A plain bool is
    // enough; promote to atomic only if a cross-thread reader appears.
    bool m_queueCommandsDuringSuspend;
};

} // namespace cec_control
