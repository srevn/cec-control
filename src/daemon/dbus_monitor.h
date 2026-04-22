#pragma once

#include <chrono>
#include <functional>
#include <systemd/sd-bus.h>

#include "../common/backoff_schedule.h"
#include "../common/event_loop.h"
#include "../common/timer_source.h"

namespace cec_control {

/**
 * Monitors logind PrepareForSleep signals over sd-bus.
 *
 * Integrates with the unified EventLoop: attach() registers the bus fd
 * and a local timerfd (for sd_bus_get_timeout()) with the loop; every
 * loop callback processes pending bus activity and refreshes the fd
 * mask plus the timer. No dedicated thread, no shutdown pipe.
 *
 * On a runtime bus failure (logind or dbus-daemon restart, explicit
 * hang-up) the monitor transitions into a Reconnecting state: loop
 * registration of the bus fd is dropped, a backoff timer arms, and
 * each firing retries sd_bus_default_system + add_match + take-lock.
 * CEC-side work keeps running throughout. If every retry in
 * m_reconnectSchedule fails, the monitor moves to Disabled and power
 * monitoring stays off for the rest of the session (no further
 * periodic log noise).
 *
 * ---------------------------------------------------------------
 * sd-bus integration invariant — never sd_bus_call* on the loop.
 * ---------------------------------------------------------------
 *
 * Runtime bus operations MUST be asynchronous (sd_bus_send or
 * sd_bus_call_method_async). sd_bus_call_method internally uses
 * sd_bus_process_priority() to wait for its reply, which dispatches
 * only the specific reply it is waiting for — signals that arrive
 * during the wait are read into sd-bus's internal receive queue but
 * their match callbacks are NOT fired. By the time the sync call
 * returns, the kernel socket is drained (so epoll will not wake on
 * the bus fd) and the signals are stranded inside libsystemd. They
 * sit there until some unrelated event wakes the loop, which in the
 * suspend path means after the kernel has already slept and resumed.
 * That's exactly the regression that keeps coming back every time
 * someone reaches for sd_bus_call_method because it's convenient.
 *
 * Async paths in this class:
 *   - suspendSystem(): sd_bus_call_method_async with a floating slot.
 *     The reply carries no payload we care about, but routing it
 *     through onSuspendReply surfaces any authorization/bus errors.
 *   - takeInhibitLock() at runtime: sd_bus_call_method_async with
 *     m_inhibitSlot tracked so we can cancel on shutdown; the reply
 *     (onInhibitReply) dups the inhibitor fd into m_inhibitFd.
 *
 * The single exception is the initial inhibit-lock acquisition inside
 * initialize(): it runs before attach(), so there is no event loop to
 * carry the reply. A synchronous call is safe there because no signals
 * could yet be queued against the newly-installed match.
 *
 * Single-threaded ownership: every sd-bus operation runs on the thread
 * that calls run() on the EventLoop. Other threads that need to emit
 * bus calls (e.g. the TV-standby worker calling suspendSystem()) must
 * post their work through MainThreadWork.
 */
class DBusMonitor {
public:
    enum class PowerState {
        Suspending,
        Resuming
    };

    using PowerStateCallback = std::function<void(PowerState)>;

    DBusMonitor();
    ~DBusMonitor();

    DBusMonitor(const DBusMonitor&) = delete;
    DBusMonitor& operator=(const DBusMonitor&) = delete;

    /**
     * Connect to the system bus, subscribe to PrepareForSleep, and take
     * the initial delay-inhibitor lock. Does not register with an event
     * loop yet; call attach() for that.
     */
    [[nodiscard]] bool initialize();

    /** Replace the callback invoked on Suspending / Resuming transitions. */
    void setCallback(PowerStateCallback cb);

    /**
     * Register the bus fd and internal timer with @p loop. The loop
     * must outlive this object (or be detach()ed first). Fails if
     * initialize() has not run.
     */
    [[nodiscard]] bool attach(EventLoop& loop);

    /** Remove from the event loop. Idempotent. */
    void detach();

    /**
     * Release the inhibit lock and unref the bus connection. Idempotent;
     * safe to call after detach() or without ever having attached.
     */
    void stop();

    /**
     * (Re)take a delay inhibitor from logind. Must run on the main
     * thread (sd-bus calls are not thread-safe with us).
     *
     * Before attach() — the one-shot path used only from initialize() —
     * the request is issued synchronously and the fd is stored before
     * return. At runtime (post-attach) the request is scheduled via
     * sd_bus_call_method_async and the reply dups the fd into
     * m_inhibitFd asynchronously; the return value indicates only that
     * the call was queued. Harmless if we already hold a lock or a
     * prior request is still pending.
     */
    bool takeInhibitLock();

    /**
     * Release our inhibit lock by closing the fd. Does not touch sd-bus;
     * safe to call from any thread. Idempotent.
     */
    bool releaseInhibitLock() noexcept;

    /**
     * Ask logind to suspend the system. Must run on the main thread.
     * Fires sd_bus_call_method_async and returns immediately; the
     * PrepareForSleep signals arrive later through the normal
     * processBus() dispatch path. Returns true if the call was queued
     * successfully.
     */
    bool suspendSystem();

private:
    /**
     * Lifecycle of the sd-bus connection from this monitor's point of
     * view. Starts Operational; a fatal sd_bus_process / sd_bus_get_fd
     * error transitions to Reconnecting (bus torn down, backoff timer
     * armed). Successful reconnect returns to Operational; exhausting
     * m_reconnectSchedule promotes to Disabled (no further bus activity
     * this session — CEC work continues regardless).
     */
    enum class BusState {
        Operational,
        Reconnecting,
        Disabled
    };

    /** Run sd_bus_process until drained; collect any deferred work. */
    void processBus();

    /** Re-query sd-bus for its desired event mask and next timeout. */
    void updateLoopRegistration();

    /** Loop handlers. */
    void onBusReadable();
    void onTimerFire();
    void onReconnectTimer();

    /**
     * Drop the bus connection on detection of a fatal error, release
     * the inhibit lock, unregister the bus fd from the loop, and arm
     * the reconnect timer with the first entry of m_reconnectSchedule.
     */
    void handleBusDisconnect();

    /**
     * Establish a fresh sd-bus connection, re-install the signal match,
     * and retake the inhibit lock (best effort). Returns false on any
     * fatal step; leaves m_bus/m_signalSlot null on failure so the
     * caller can retry cleanly.
     */
    [[nodiscard]] bool reconnectBus();

    /** Register the freshly reconnected bus fd with m_loop. */
    [[nodiscard]] bool registerBusWithLoop();

    /** Unref the signal slot and bus; clear both pointers. */
    void tearDownBusState() noexcept;

    /** Static signal handler registered with sd-bus; forwards to m_callback. */
    static int onPrepareForSleep(
        sd_bus_message* msg,
        void* userdata,
        sd_bus_error* ret_error
    );

    /**
     * Reply handler for the async Inhibit method call. Stores the
     * dup'd inhibitor fd in m_inhibitFd, or logs a warning on error.
     * Runs on the main thread as part of a processBus() dispatch.
     */
    static int onInhibitReply(
        sd_bus_message* msg,
        void* userdata,
        sd_bus_error* ret_error
    );

    /**
     * Reply handler for the async Suspend method call. Carries no
     * payload we need, but surfaces authorization or bus errors so
     * they end up in the log instead of vanishing.
     */
    static int onSuspendReply(
        sd_bus_message* msg,
        void* userdata,
        sd_bus_error* ret_error
    );

    /** Short textual conversion for negative sd-bus return values. */
    static const char* busErrorToString(int error) noexcept;

    sd_bus* m_bus = nullptr;
    sd_bus_slot* m_signalSlot = nullptr;
    sd_bus_slot* m_inhibitSlot = nullptr;  // In-flight async Inhibit request.
    int m_inhibitFd = -1;

    EventLoop* m_loop = nullptr;
    int m_registeredBusFd = -1;     // The fd we have currently added to m_loop.
    uint32_t m_registeredMask = 0;  // Last mask passed to loop->modify.
    TimerSource m_timer;            // Carries sd-bus's internal deadline.
    TimerSource m_reconnectTimer;   // Drives the disconnect-backoff schedule.

    PowerStateCallback m_callback;

    BusState m_state = BusState::Operational;
    // Delays between reconnect attempts after a bus disconnect. Tuned so
    // a quick dbus-daemon / logind restart (sub-second downtime) is
    // caught by the first retry; a genuine outage falls onto exponential
    // backoff. Exhausting the schedule transitions to Disabled.
    BackoffSchedule m_reconnectSchedule{
        std::chrono::seconds(2),
        std::chrono::seconds(10),
        std::chrono::seconds(30),
        std::chrono::seconds(60),
    };

    // 1-based position of the pending reconnect attempt. Captured on
    // the BackoffSchedule::Attempt returned from nextDelay() when the
    // reconnect timer is armed, used by onReconnectTimer() at fire
    // time to build the "attempt N of M" log line. Zero outside an
    // active Reconnecting cycle.
    std::size_t m_currentAttemptNumber = 0;
};

} // namespace cec_control
