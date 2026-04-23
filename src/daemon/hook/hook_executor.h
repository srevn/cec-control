#pragma once

#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace cec_control {

/**
 * @class HookExecutor
 * @brief Dedicated thread that drains a queue of @c Job values and
 *        @c posix_spawn s each one. Fire-and-forget: no result is
 *        returned, no child PID is tracked beyond a debug log.
 *
 * ## Why a dedicated thread
 *
 * @c posix_spawn is usually fast but not free — on a process of this
 * size (libcec, udev, sd-bus mappings) the underlying fork can
 * momentarily stall on kernel page-table duplication. Running it on
 * its own thread keeps the main loop snappy and isolates libcec's
 * worker from subprocess launch jitter.
 *
 * ## Threading contract
 *
 *  - @c submit() is safe from any thread; in practice called from the
 *    main-thread @c CecHookSubsystem.
 *  - @c start() / @c stop() are called on the main thread.
 *  - @c stop() joins the thread. Pending @c Job values that have not
 *    yet been spawned are dropped (matches @c AdapterWorker::stop).
 *
 * Child reaping is @b not handled here — it lives in @c reapChildren()
 * in the enclosing @c hook namespace, called from the daemon's SIGCHLD
 * handler on the main thread. Keeping the reap state off this class
 * means the reap path works even when no executor exists (startup or
 * post-teardown windows) and keeps libcec's signalfd handling free of
 * the executor's lifetime assumptions.
 */
class HookExecutor {
public:
    /**
     * A single spawn request. @c env holds @c "KEY=VALUE" strings and
     * outlives the spawn call — the worker synthesises a @c char*[]
     * from the strings' @c data() pointers and passes it to
     * @c posix_spawn, which copies the pointers into the child.
     */
    struct Job {
        std::string              path;
        std::vector<std::string> env;
    };

    HookExecutor()  = default;
    ~HookExecutor();

    HookExecutor(const HookExecutor&)            = delete;
    HookExecutor& operator=(const HookExecutor&) = delete;
    HookExecutor(HookExecutor&&)                 = delete;
    HookExecutor& operator=(HookExecutor&&)      = delete;

    /** Spawn the exec thread. Idempotent; main thread only. */
    void start();

    /**
     * Signal stop; join the exec thread; drop any queued but unspawned
     * jobs. Already-spawned children are not tracked — they continue
     * running and, when they exit after the daemon has quit, are
     * reaped by @c init (pid 1). Idempotent; main thread only.
     */
    void stop();

    /**
     * Enqueue @p job. Silently dropped after @c stop(). Fire-and-
     * forget: no acknowledgement, no completion hop, no PID surfaced
     * to the caller.
     */
    void submit(Job job);

private:
    void run();

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::queue<Job>         m_jobs;
    bool                    m_stopRequested = false;
    bool                    m_started       = false;

    // Last field so the thread is joined before the synchronisation
    // primitives above are destroyed on an unexpected destruction path
    // (the normal path runs stop() first).
    std::thread m_thread;
};

namespace hook {

/**
 * Reap zombie children with @c waitpid(-1, ..., WNOHANG) until the
 * queue drains. Safe to call from any thread but in practice the main
 * thread's SIGCHLD handler. No state is tracked across calls — the
 * @c WNOHANG loop naturally absorbs "already reaped" and "nothing to
 * reap" outcomes.
 *
 * Deliberately a free function so a SIGCHLD arriving before the
 * executor exists (startup) or after it has been destroyed (shutdown)
 * is still reaped without a lifetime lookup on @c HookExecutor.
 */
void reapChildren() noexcept;

} // namespace hook

} // namespace cec_control
