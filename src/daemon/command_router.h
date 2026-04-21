#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "../common/messages.h"
#include "cec/libcec_adapter.h"
#include "command_throttler.h"
#include "thread_pool.h"

namespace cec_control {

/**
 * @class CommandRouter
 * @brief Owns the CEC adapter and every piece of state that pivots on the
 *        suspend / resume lifecycle.
 *
 * Locking discipline:
 *
 *  - @ref m_stateMutex guards lifecycle state only — the suspend flag,
 *    the shutdown flag, and the queue of commands parked during suspend.
 *    It is held briefly: a dispatch's phase-1 state check, the flag
 *    flip in @c suspend() / @c resume() / @c shutdown(), and the queue
 *    take-out on the resume path. It is NEVER held across a libcec
 *    call or across a throttler sleep.
 *
 *  - libcec access is serialised inside @c LibCecAdapter via its own
 *    adapter mutex; the router does not replicate that serialisation.
 *
 *  - @c CommandThrottler is thread-safe on its own (atomics + CAS)
 *    and its sleeps are outside every lock.
 *
 * @c dispatch() therefore splits into two phases: phase-1 reads and
 * mutates router state under @c m_stateMutex (queueing, shutdown /
 * suspend gates, CMD_RESTART_ADAPTER scheduling); phase-2 runs the
 * command through the adapter and throttler with no router lock held.
 * The key property is that @c suspend() never waits on throttler
 * retries: it flips the suspend flag under @c m_stateMutex and releases
 * the lock before touching the adapter. Any in-flight dispatch that
 * already passed phase-1 finishes against the adapter concurrently
 * with @c suspend()'s @c closeConnection() — the adapter mutex
 * naturally serialises the two.
 *
 * The router is deliberately ignorant of the D-Bus inhibit lock,
 * signals, and the run-loop. The daemon orchestrates those around
 * calls to @c suspend() / @c resume(); the router only does CEC work.
 */
class CommandRouter {
public:
    /**
     * Fully populated options struct. DaemonBootstrap builds this from the
     * config file; the router forwards adapter/throttler sub-structs into
     * their respective components at construction and does no config I/O
     * itself.
     */
    struct Options {
        bool scanDevicesAtStartup = false;
        bool queueCommandsDuringSuspend = true;
        bool autoStandbyEnabled = false;
        LibCecAdapter::Options adapter;
        CommandThrottler::Options throttler;
    };

    /**
     * Outbound hooks fired by the router. The daemon supplies both at
     * construction and never rewires them. Both run on the router's
     * caller thread — libcec's internal thread for @c onConnectionLost
     * and @c onSuspendRequested alike — so implementations MUST be safe
     * to invoke from an arbitrary thread and MUST NOT block (the libcec
     * callback thread cannot be stalled without risking deadlock against
     * a concurrent adapter call). The expected shape is a thread-safe
     * post onto the daemon's main-thread work queue. Either member may
     * be empty; the router silently no-ops in that case.
     */
    struct Callbacks {
        /** libcec dropped the adapter (fires on a libcec-owned thread). */
        std::function<void()> onConnectionLost;
        /** TV-standby + auto-standby policy agree the system should sleep. */
        std::function<void()> onSuspendRequested;
    };

    /**
     * @param options     Fully-populated configuration options.
     * @param threadPool  Pool for background tasks (async adapter restart).
     *                    Must be non-null and outlive @c this;
     *                    DaemonBootstrap guarantees both.
     * @param callbacks   Install-once outbound hooks; see Callbacks.
     */
    CommandRouter(Options options,
                  std::shared_ptr<ThreadPool> threadPool,
                  Callbacks callbacks);
    ~CommandRouter();

    CommandRouter(const CommandRouter&) = delete;
    CommandRouter& operator=(const CommandRouter&) = delete;

    [[nodiscard]] bool initialize();

    /**
     * Close the adapter and drop any queued commands. Idempotent: safe
     * to call multiple times (explicit shutdown plus destructor path).
     */
    void shutdown();

    /**
     * Synchronous command dispatch. Thread-safe.
     *
     * Splits internally into a brief phase-1 state check under
     * @c m_stateMutex and a phase-2 command execution that holds no
     * router-level lock (libcec is serialised by the adapter's own
     * mutex, the throttler by its own atomics).
     *
     * While suspended, queueable commands are parked for a post-resume
     * drain and return RESP_SUCCESS ("accepted for when the system
     * wakes"); non-queueable commands return RESP_ERROR.
     * CMD_RESTART_ADAPTER is handled in phase-1 and rejected if the
     * router is shut down or suspended — both states imply the adapter
     * is deliberately closed and a reopen would race the other
     * transition.
     */
    Message dispatch(const Message& command);

    /**
     * Reconnect if the adapter is not currently connected. No-op when
     * already connected, and a no-op with a debug log while suspended
     * or shut down (both states deliberately close the adapter).
     * Thread-safe.
     *
     * The state check and the reopen are not atomic with respect to a
     * concurrent @c suspend() / @c shutdown(). In the unlikely race, the
     * adapter is reopened and immediately closed again on the next
     * lifecycle tick; the adapter mutex serialises the two calls so no
     * inconsistent state is observable from outside.
     */
    [[nodiscard]] bool reconnect();

    /**
     * Issue pre-sleep CEC actions and close the adapter. Synchronous;
     * returns once the adapter is fully down. Safe to call while
     * already suspended (returns immediately).
     *
     * Does not wait on in-flight throttler retries. The suspend flag
     * is flipped under @c m_stateMutex — new dispatches see it and
     * bail immediately — and the lock is released before the adapter
     * close sequence runs. An already-running phase-2 dispatch
     * completes against the adapter concurrently (serialised by the
     * adapter mutex, ms-granularity) and then sees its next iteration
     * fail cleanly as the connection drops.
     */
    void suspend();

    /**
     * Reconnect the adapter, power on configured devices, clear the
     * suspend flag, and replay any commands queued during suspend.
     *
     * Ordering: commands queued during suspend are replayed in the
     * order they arrived. New dispatches arriving after the suspend
     * flag is cleared may interleave with the replay at the adapter
     * level — the adapter mutex serialises each libcec call, but
     * replayed and newly-arriving commands race for the next adapter
     * slot. Acceptable because commands at this layer are idempotent
     * (volume up/down, power on/off, source selection) and because
     * preserving strict FIFO would require holding @c m_stateMutex
     * across every throttler sleep in the drain — reintroducing the
     * long-hold pattern that this lock discipline deliberately avoids.
     *
     * Returns once the drain has been fully attempted. Safe to call
     * while not suspended.
     */
    void resume();

    [[nodiscard]] bool isAdapterValid() const;
    [[nodiscard]] bool isSuspended() const;

private:
    // Components. Direct members (not shared_ptr) — the router is the sole
    // owner and its lifetime always encloses theirs.
    LibCecAdapter m_adapter;
    CommandThrottler m_throttler;

    // Configuration. Immutable after construction.
    const Options m_options;

    // Background pool for async tasks. Must outlive this router.
    std::shared_ptr<ThreadPool> m_threadPool;

    // Narrow lock protecting lifecycle flags and the suspend queue. See
    // the class-level doc-comment for scope and what deliberately lives
    // outside this lock.
    mutable std::mutex m_stateMutex;

    // Lifecycle flags. Writes happen under m_stateMutex so the queue
    // mutation stays coherent with the flag flip; reads are lock-free
    // so observers (dispatch phase-1 elsewhere, daemon state queries,
    // the scheduled-restart task) do not serialise on unrelated writers.
    std::atomic<bool> m_suspended{false};
    std::atomic<bool> m_shutdownComplete{false};

    // Commands parked while suspended. Guarded by m_stateMutex.
    std::vector<Message> m_queuedCommands;

    // Policy: suspend the PC when the TV signals standby. Atomic so the
    // TV-standby callback (libCEC's thread) can read it without waiting
    // on any router-side state mutex.
    std::atomic<bool> m_autoStandbyEnabled;

    // Suspend dispatch target — install-once at construction, invoked
    // inline from onTvStandby (see Callbacks contract). Never reassigned.
    const std::function<void()> m_suspendCallback;

    /**
     * Phase-1 helper for @c CMD_RESTART_ADAPTER. Caller must hold
     * @c m_stateMutex. Returns the wire response to send back.
     */
    Message handleRestartLocked(const Message& command);

    /**
     * Phase-1 helper for commands arriving while suspended. Caller
     * must hold @c m_stateMutex. Either queues the command for later
     * replay or rejects it, per its queueability spec.
     */
    Message handleSuspendedLocked(const Message& command);

    /**
     * Phase-2 body: dispatches @p command through the adapter and
     * throttler with no router lock held. Called both from @c dispatch()
     * and from @c resume()'s drain.
     */
    Message executeCommand(const Message& command);

    /**
     * Schedule a fresh adapter reinit on the thread pool. The submitted
     * task re-checks @c m_shutdownComplete / @c m_suspended under
     * @c m_stateMutex and aborts if either is set, then performs the
     * reopen with no router-side lock held. A concurrent suspend or
     * shutdown that transitions between the check and the reopen is
     * tolerated: the reopen runs to completion and the following
     * @c suspend() / @c shutdown() closes the adapter again. The
     * adapter mutex serialises the two calls internally.
     */
    void scheduleRestart();

    /** Hook invoked by the adapter when the TV signals STANDBY. */
    void onTvStandby();
};

} // namespace cec_control
