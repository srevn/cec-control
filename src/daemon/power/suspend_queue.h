#pragma once

#include <vector>

#include "../../common/messages.h"

namespace cec_control {

/**
 * @class SuspendQueue
 * @brief Pairs the "router is suspended" flag with the vector of
 *        commands parked during that suspension.
 *
 * This is the small amount of state that pivots on the lifecycle's
 * suspend / resume cycle. Ownership lives here so the lifecycle does
 * not carry a loose flag/vector pair and coordinate their
 * transitions by hand; the transitions are named (@c enterSuspended,
 * @c exitSuspended, @c push, @c drain) and the flag is
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
 *    @c CommandDispatcher; @c push is a pure mechanism that checks
 *    the flag and appends.
 *  - Shutdown gating. Distinct state with a distinct lifecycle,
 *    handled by @c AdapterLifecycle directly.
 *
 * ## Thread-safety
 *
 * Every public method must be called on the main thread. The single-
 * threaded access model of @c AdapterLifecycle (the sole owner)
 * provides the ordering guarantees; no internal synchronisation is
 * needed.
 */
class SuspendQueue {
public:
    SuspendQueue() = default;

    SuspendQueue(const SuspendQueue&) = delete;
    SuspendQueue& operator=(const SuspendQueue&) = delete;

    /** Enter the suspended state. Subsequent @c push calls append. */
    void enterSuspended() noexcept;

    /**
     * Exit the suspended state. Subsequent @c push calls no-op.
     * Does not drain — the caller drains separately so the queued
     * messages can be handed to the replay path before this flag
     * flip makes future dispatches race the drain.
     */
    void exitSuspended() noexcept;

    /** Read the suspend flag. */
    [[nodiscard]] bool isSuspended() const noexcept;

    /**
     * Append @p command if currently suspended; otherwise no-op. The
     * check is internal so this primitive is safe to call without a
     * prior @c isSuspended test.
     */
    void push(const Message& command);

    /** Take the queued messages; leave the queue empty. */
    [[nodiscard]] std::vector<Message> drain();

private:
    bool                 m_suspended = false;
    std::vector<Message> m_queued;
};

} // namespace cec_control
