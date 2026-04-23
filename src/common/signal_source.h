#pragma once

#include <initializer_list>
#include <optional>
#include <signal.h>
#include <sys/signalfd.h>

namespace cec_control {

/**
 * signalfd wrapper that blocks the given signals on the calling thread
 * and delivers them via a readable file descriptor.
 *
 * Construct this on the main thread before spawning any worker threads.
 * Workers inherit the blocked-signal mask, so termination signals reach
 * only the main thread via readOne() — no async signal handler, no
 * singleton pointer, no cross-thread dance.
 *
 * @c SIGCHLD is the expected channel for reaping hook-script children
 * spawned via @c HookExecutor: add it to the constructor's signal set
 * and dispatch to @c hook::reapChildren from the signalfd handler.
 * Same inheritance story — the hook executor thread starts after this
 * source is constructed, so it inherits SIG_BLOCK for SIGCHLD and the
 * child-exit notification lands on the main thread.
 *
 * On destruction the signal mask is left as-is. That is deliberate: the
 * daemon typically wants an operator's second signal during a stuck
 * shutdown to reach the kernel default action (SIG_DFL handles
 * termination signals by killing the process). To restore, construct a
 * SignalSource with the same signals in a scope, then destroy it and
 * call pthread_sigmask(SIG_UNBLOCK, mask, nullptr) yourself — the
 * defaults are intentional here and we do not second-guess callers.
 */
class SignalSource {
public:
    /**
     * Block the listed signals (pthread_sigmask SIG_BLOCK on the current
     * thread) and create a non-blocking, close-on-exec signalfd to
     * receive them.
     */
    explicit SignalSource(std::initializer_list<int> signals);
    ~SignalSource();

    SignalSource(const SignalSource&) = delete;
    SignalSource& operator=(const SignalSource&) = delete;

    /** True if the signalfd was created successfully. */
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    /** The fd to register with EventLoop (READ). */
    int fd() const noexcept { return m_fd; }

    /**
     * Read the next pending signalfd_siginfo, if any. Drains one
     * record per call; callers should invoke in a loop until nullopt to
     * fully drain a batch of queued signals.
     */
    std::optional<signalfd_siginfo> readOne() noexcept;

private:
    int m_fd = -1;
    sigset_t m_mask;
};

} // namespace cec_control
