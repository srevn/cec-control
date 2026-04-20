#pragma once

#include <atomic>
#include <memory>
#include <signal.h>
#include <thread>

#include "command_router.h"
#include "socket_server.h"
#include "dbus_monitor.h"
#include "thread_pool.h"
#include "../common/logger.h"

namespace cec_control {

/**
 * @class CECDaemon
 * @brief Top-level orchestrator: signals, main-loop wake, DBus + router
 *        coordination, socket server lifetime.
 *
 * The daemon itself carries very little state. Suspend/resume bookkeeping
 * lives in the CommandRouter; adapter liveness lives in CECAdapter; connection
 * lifetime lives in SocketServer. What remains here is the glue: wire the
 * signal handler into the run-loop via an eventfd, route socket commands to
 * the right subsystem, and coordinate suspend/resume between the router and
 * the D-Bus inhibit lock.
 */
class CECDaemon {
public:
    /**
     * @brief Daemon-level lifecycle knobs. Anything CEC-specific (devices,
     * throttling, auto-standby) lives on CommandRouter::Options; these are
     * the handful of switches that sit above the router.
     */
    struct Options {
        bool enablePowerMonitor = true;
    };

    CECDaemon(Options daemonOptions, CommandRouter::Options routerOptions);
    ~CECDaemon();

    CECDaemon(const CECDaemon&) = delete;
    CECDaemon& operator=(const CECDaemon&) = delete;

    /** Bring up the router, socket server, signal handlers, DBus monitor. */
    [[nodiscard]] bool start();

    /** Signal the run-loop to exit; tear everything down. Idempotent. */
    void stop();

    /** Block on the main-loop eventfd until signalled to exit. */
    void run();

    /**
     * Async-signal-safe static handler. Kept for the current single-threaded
     * signal delivery model; Phase E collapses this onto a signalfd inside
     * the unified poller and deletes the handler + singleton entirely.
     */
    static void signalHandler(int signal);
    static CECDaemon* getInstance() noexcept { return s_instance; }

    /** Pre-sleep coordination: router suspend + DBus inhibit release. */
    void onSuspend();

    /** Post-wake coordination: router resume + optional 10s retry fallback. */
    void onResume();

private:
    /** Connection-lost callback invoked by the router on a libCEC thread. */
    void onConnectionLost();

    /** Main-loop side of connection-lost: run the reconnect on our thread. */
    void onConnectionLostEvent();

    /** Route a wire message to the appropriate handler. */
    Message handleCommand(const Message& command);

    /** Install SIGINT/SIGTERM/SIGHUP handlers via sigaction. */
    void setupSignalHandlers();

    /** Restore the affected signals to SIG_DFL. Idempotent. */
    void teardownSignalHandlers();

    /** Wake the main loop by writing to m_wakeFd. Async-signal-safe; best-effort. */
    void wakeMainLoop() noexcept;

    /** Drain any accumulated counter writes from m_wakeFd. */
    void drainWakeFd() noexcept;

    /** React to a power state change surfaced by DBusMonitor. */
    void handlePowerStateChange(DBusMonitor::PowerState state);

    /** Create and start the DBus monitor. Returns true on success. */
    bool setupPowerMonitor();

    // Core components
    CommandRouter::Options m_routerOptions;  // staged until start()
    std::unique_ptr<CommandRouter> m_router;
    std::unique_ptr<SocketServer> m_socketServer;
    std::unique_ptr<DBusMonitor> m_dbusMonitor;
    std::shared_ptr<ThreadPool> m_threadPool;

    // Run-loop state
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connectionLost{false};

    // Counts how many termination signals have been received. The third one
    // escalates to _exit() in the signal handler.
    std::atomic<int> m_signalCount{0};

    // eventfd that the signal handler and onConnectionLost() write to in
    // order to wake the main loop. Created in start() before signal handlers
    // are installed; closed in stop() after handlers are restored to SIG_DFL.
    int m_wakeFd{-1};

    Options m_options;

    static CECDaemon* s_instance;
};

} // namespace cec_control
