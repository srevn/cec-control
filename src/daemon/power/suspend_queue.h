#pragma once

#include <atomic>
#include <vector>

#include "../../common/messages.h"

namespace cec_control {

/**
 * @class SuspendQueue
 * @brief Pairs the "router is suspended" flag with the vector of
 *        commands parked during that suspension.
 *
 * This is the small amount of state that pivots on the router's
 * suspend / resume cycle. Ownership lives here so the router does
 * not carry a loose (atomic, vector) pair and coordinate their
 * transitions by hand; the transitions are named (@c enterSuspended,
 * @c exitSuspended, @c tryPush, @c drain) and the atomic flag is
 * inaccessible except through @c isSuspended.
 *
 * ## Ownership boundary
 *
 * Owned: whether we are currently queueing (the flag) and the
 * queued messages themselves.
 *
 * Not owned:
 *  - Queueing policy. Whether a given command is queueable
 *    (per-command spec from @c kCommands) and whether queueing is
 *    enabled at all (@c queueCommandsDuringSuspend config) stay on
 *    the caller; @c tryPush is a pure mechanism that checks the
 *    flag and appends.
 *  - Shutdown gating. Distinct state with a distinct lifecycle,
 *    handled by @c CommandRouter directly.
 *
 * ## Thread-safety
 *
 *  - @c isSuspended is a lock-free acquire load on an internal
 *    atomic. Callers on non-main threads (notably the daemon's
 *    late-reconnect task on the thread pool) can observe the flag
 *    without taking the router's state mutex.
 *  - @c enterSuspended, @c exitSuspended, @c tryPush and @c drain
 *    are NOT internally synchronised. The caller must hold an
 *    external lock across them so the flag flip and the vector
 *    mutation remain coherent: a dispatch that observes
 *    @c isSuspended() == true and then calls @c tryPush must not
 *    race an @c exitSuspended that drops the queue.
 *
 * @c CommandRouter holds @c m_stateMutex for every path that
 * mutates here, which satisfies the contract.
 */
class SuspendQueue {
public:
    SuspendQueue() = default;

    SuspendQueue(const SuspendQueue&) = delete;
    SuspendQueue& operator=(const SuspendQueue&) = delete;

    /** Enter the suspended state. Subsequent @c tryPush calls append. */
    void enterSuspended() noexcept;

    /**
     * Exit the suspended state. Subsequent @c tryPush calls no-op.
     * Does not drain — the caller drains separately so the queued
     * messages can be handed to the replay path before this flag
     * flip makes future dispatches race the drain.
     */
    void exitSuspended() noexcept;

    /** Lock-free read of the suspend flag. */
    [[nodiscard]] bool isSuspended() const noexcept;

    /**
     * If currently suspended, append @p command and return true;
     * otherwise no-op and return false. The check is internal so
     * this primitive is safe to call without a prior @c isSuspended
     * test.
     */
    [[nodiscard]] bool tryPush(const Message& command);

    /** Take the queued messages; leave the queue empty. */
    [[nodiscard]] std::vector<Message> drain();

private:
    std::atomic<bool>    m_suspended{false};
    std::vector<Message> m_queued;
};

} // namespace cec_control
