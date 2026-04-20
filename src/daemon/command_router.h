#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "../common/messages.h"
#include "cec_adapter.h"
#include "command_throttler.h"
#include "thread_pool.h"

namespace cec_control {

/**
 * @class CommandRouter
 * @brief Owns the CEC adapter and every piece of state that pivots on the
 *        suspend / resume lifecycle.
 *
 * Public methods acquire @ref m_routerMutex exactly once; libCEC is
 * single-threaded and this is the sole point of serialisation. The suspend
 * flag, the queue of commands parked during suspend, and the auto-standby
 * policy all live here rather than being smeared across the daemon, so a
 * single lock covers every transition.
 *
 * The router is deliberately ignorant of the D-Bus inhibit lock, signals, and
 * the run-loop. The daemon orchestrates those around calls to suspend() and
 * resume(); the router only does CEC work.
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
        CECAdapter::Options adapter;
        CommandThrottler::Options throttler;
    };

    /**
     * @param options     Fully-populated configuration options.
     * @param threadPool  Pool for background tasks (async adapter restart,
     *                    TV-standby suspend dispatch). Must be non-null and
     *                    outlive @c this; DaemonBootstrap guarantees both.
     */
    CommandRouter(Options options, std::shared_ptr<ThreadPool> threadPool);
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
     * While suspended, queueable commands are parked for a post-resume drain
     * and return RESP_SUCCESS ("accepted for when the system wakes");
     * non-queueable commands (except CMD_RESTART_ADAPTER) return RESP_ERROR.
     * CMD_RESTART_ADAPTER is always scheduled so an operator can force a
     * reinit even mid-suspend.
     */
    Message dispatch(const Message& command);

    /**
     * Reconnect if the adapter is not currently connected. No-op when already
     * connected, and a no-op with a debug log while suspended (the suspend
     * path deliberately closes the adapter). Thread-safe.
     */
    [[nodiscard]] bool reconnect();

    /**
     * Issue pre-sleep CEC actions and close the adapter. Synchronous;
     * returns once the adapter is fully down. Safe to call while already
     * suspended (returns immediately).
     */
    void suspend();

    /**
     * Reconnect the adapter, power on configured devices, clear the suspend
     * flag, and drain any commands queued during suspend. Drain runs inline
     * under the router mutex so queued commands execute as a contiguous
     * block; new dispatches block on the mutex until the drain completes.
     *
     * Returns once the drain is done. Safe to call while not suspended.
     */
    void resume();

    void setConnectionLostCallback(std::function<void()> callback);

    /**
     * Invoked when the router concludes the system should suspend (TV
     * signalled standby and auto-standby is enabled). Daemon wires this
     * to the D-Bus Suspend method; the actual success/failure of the
     * Suspend call is logged by the DBus monitor itself, so the callback
     * is fire-and-forget. Called on a thread-pool worker, never inline
     * from dispatch().
     */
    void setSuspendCallback(std::function<void()> callback);

    [[nodiscard]] bool isAdapterValid() const;
    [[nodiscard]] bool isSuspended() const;

private:
    // Components. Direct members (not shared_ptr) — the router is the sole
    // owner and its lifetime always encloses theirs.
    CECAdapter m_adapter;
    CommandThrottler m_throttler;

    // Configuration. Immutable after construction.
    const Options m_options;

    // Background pool for async tasks. Must outlive this router.
    std::shared_ptr<ThreadPool> m_threadPool;

    // Sole lock protecting adapter access, suspend state, and queue.
    mutable std::mutex m_routerMutex;

    // Suspend lifecycle state (guarded by m_routerMutex).
    bool m_suspended = false;
    bool m_shutdownComplete = false;
    std::vector<Message> m_queuedCommands;

    // Policy: suspend the PC when the TV signals standby. Atomic because the
    // TV standby callback runs on libCEC's thread and must not contend on
    // m_routerMutex (which can be held by dispatch() mid-command).
    std::atomic<bool> m_autoStandbyEnabled;

    // Suspend dispatch target installed by the daemon.
    std::function<void()> m_suspendCallback;

    /** Dispatch body. Caller must hold m_routerMutex. */
    Message dispatchLocked(const Message& command);

    /** Schedule a fresh adapter reinit on the thread pool. */
    void scheduleRestart();

    /** Hook invoked by the adapter when the TV signals STANDBY. */
    void onTvStandby();
};

} // namespace cec_control
