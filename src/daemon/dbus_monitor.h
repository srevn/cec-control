#pragma once

#include <cstddef>
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
 * On a runtime bus failure (logind or dbus-daemon restart, explicit
 * hang-up) the monitor transitions into a Reconnecting state: loop
 * registration of the bus fd is dropped, a backoff timer arms, and
 * each firing retries sd_bus_default_system + add_match + take-lock.
 * CEC-side work keeps running throughout. If every retry in
 * kReconnectSchedule fails, the monitor moves to Disabled and power
 * monitoring stays off for the rest of the session (no further
 * periodic log noise).
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
    /**
     * Lifecycle of the sd-bus connection from this monitor's point of
     * view. Starts Operational; a fatal sd_bus_process / sd_bus_get_fd
     * error transitions to Reconnecting (bus torn down, backoff timer
     * armed). Successful reconnect returns to Operational; exhausting
     * kReconnectSchedule promotes to Disabled (no further bus activity
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
     * the reconnect timer with the first entry of kReconnectSchedule.
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
    TimerSource m_timer;           // Carries sd-bus's internal deadline.
    TimerSource m_reconnectTimer;  // Drives the disconnect-backoff schedule.

    PowerStateCallback m_callback;

    BusState m_state = BusState::Operational;
    std::size_t m_reconnectAttempts = 0;
};

} // namespace cec_control
