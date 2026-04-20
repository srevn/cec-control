#pragma once

#include <functional>
#include <systemd/sd-bus.h>

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
     * thread (sd-bus calls are not thread-safe with us). Harmless if
     * we already hold a lock.
     */
    bool takeInhibitLock();

    /**
     * Release our inhibit lock by closing the fd. Does not touch sd-bus;
     * safe to call from any thread. Idempotent.
     */
    bool releaseInhibitLock() noexcept;

    /**
     * Ask logind to suspend the system. Must run on the main thread.
     * Returns true on successful dispatch; the actual PrepareForSleep
     * signal arrives later via the main loop.
     */
    bool suspendSystem();

private:
    /** Run sd_bus_process until drained; collect any deferred work. */
    void processBus();

    /** Re-query sd-bus for its desired event mask and next timeout. */
    void updateLoopRegistration();

    /** Loop handlers. */
    void onBusReadable();
    void onTimerFire();

    /** Static signal handler registered with sd-bus; forwards to m_callback. */
    static int onPrepareForSleep(sd_bus_message* msg, void* userdata,
                                 sd_bus_error* ret_error);

    /** Short textual conversion for negative sd-bus return values. */
    static const char* busErrorToString(int error) noexcept;

    sd_bus* m_bus = nullptr;
    sd_bus_slot* m_signalSlot = nullptr;
    int m_inhibitFd = -1;

    EventLoop* m_loop = nullptr;
    int m_registeredBusFd = -1;    // The fd we have currently added to m_loop.
    uint32_t m_registeredMask = 0;  // Last mask passed to loop->modify.
    TimerSource m_timer;

    PowerStateCallback m_callback;

    /**
     * Set by onPrepareForSleep(false) to request a replacement inhibit
     * lock after the outer sd_bus_process drain returns. Taking the
     * lock re-enters sd_bus_call_method; deferring keeps that out of
     * the dispatch callback stack.
     */
    bool m_retakeInhibitOnNextIteration = false;
};

} // namespace cec_control
