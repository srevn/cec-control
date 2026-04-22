#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "adapter_interface.h"

namespace cec_control {

/**
 * @class AdapterWorker
 * @brief Actor that owns an @c ICecAdapter and is the sole thread
 *        authorised to talk to it.
 *
 * Every libcec call travels through @c submit(). The worker thread
 * dequeues jobs FIFO and runs each with exclusive adapter access.
 *
 * Ownership of the adapter is co-terminous with the worker: the adapter
 * is closed on the worker thread as its exit step, and the
 * @c unique_ptr is then released on the caller (main) thread.
 *
 * ## Why this class exists
 *
 * Before this refactor, CEC-side blocking work was serialised by a
 * mutex inside the adapter. That works but costs: every caller contends
 * on the mutex; sleeps and syscalls happen under it; the lock scope
 * sprawls across every public method. Moving to an actor collapses the
 * serialisation model from "many threads, one lock" to "one thread,
 * one queue" — the same guarantee without the contention surface and
 * with no opportunity to accidentally hold the lock across a libcec
 * call.
 *
 * ## Thread-safety contract
 *
 *  - @c submit() is safe to call from any thread. After @c stop() it
 *    silently drops; there is no back-pressure signal.
 *  - @c start() / @c stop() must be called on the main thread (the
 *    thread that constructed and will destruct this object).
 *  - @c isAdapterConnected() is main-thread only. The underlying
 *    @c ICecAdapter::isConnected hint is itself atomic; the wrapping
 *    unique_ptr is stable during the worker's observable lifetime
 *    because it is only reset from @c stop() after the worker thread
 *    has been joined.
 *
 * ## Non-goals
 *
 * This class does @b not own a main-thread work queue. Jobs that need
 * to deliver a result back to the main thread call @c
 * MainThreadWork::post from inside the job; @c AdapterWorker has no
 * header dependency on @c MainThreadWork and no completion hook
 * machinery of its own.
 */
class AdapterWorker {
public:
    /**
     * Unit of blocking adapter work. The reference is valid for the
     * duration of the call only; do not store it or pass it to another
     * thread.
     */
    using Job = std::function<void(ICecAdapter&)>;

    /**
     * Take ownership of an adapter. Typical pattern: the caller runs
     * @c initialize() and @c openConnection() on the main thread
     * (libcec's Open is thread-identity-agnostic) and hands the opened
     * adapter in. The worker is the sole thread that invokes any other
     * adapter method; it closes the connection as its exit step.
     */
    explicit AdapterWorker(std::unique_ptr<ICecAdapter> adapter) noexcept;

    /** Blocks on @c stop() if the worker is still running. */
    ~AdapterWorker();

    AdapterWorker(const AdapterWorker&)            = delete;
    AdapterWorker& operator=(const AdapterWorker&) = delete;
    AdapterWorker(AdapterWorker&&)                 = delete;
    AdapterWorker& operator=(AdapterWorker&&)      = delete;

    /** Spawn the worker thread. Idempotent. */
    void start();

    /**
     * Signal stop; the worker finishes the in-flight job, drops any
     * queued jobs, closes the adapter on the worker thread, and exits.
     * Blocks on join. Subsequent @c submit() calls are silently dropped.
     * Idempotent; safe to call from the destructor.
     */
    void stop();

    /**
     * Enqueue a job. Main-thread callers dominate but any thread is
     * safe. Silently drops after @c stop() — the caller must treat this
     * as fire-and-forget unless the job itself arranges a completion
     * hop.
     */
    void submit(Job job);

    /**
     * Main-thread cheap read of the adapter's connection hint. Returns
     * false once the adapter has been released by @c stop().
     */
    [[nodiscard]] bool isAdapterConnected() const noexcept;

private:
    void run();

    std::unique_ptr<ICecAdapter> m_adapter;

    mutable std::mutex      m_mutex;
    std::condition_variable m_cv;
    std::queue<Job>         m_jobs;
    bool                    m_stopRequested = false;
    bool                    m_started       = false;

    // m_thread is the last field so that on normal destruction the
    // thread is joined before the synchronisation primitives above are
    // destroyed.
    std::thread m_thread;
};

} // namespace cec_control
