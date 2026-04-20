#pragma once

#include <functional>
#include <memory>
#include <mutex>

#include "../common/messages.h"
#include "cec_adapter.h"
#include "command_throttler.h"
#include "thread_pool.h"

namespace cec_control {

/**
 * @class CECManager
 * @brief Owns the CEC adapter and serialises every command against it.
 *
 * Public methods acquire @ref m_managerMutex exactly once; libCEC is
 * single-threaded and this is the single point of serialisation. The per-
 * device operations (power, volume, source, scan) live as anonymous-namespace
 * free functions in the translation unit — a separate class duplicated this
 * lock for no benefit and had exactly one caller.
 */
class CECManager {
public:
    /**
     * @brief Fully populated options struct.
     *
     * DaemonBootstrap builds this from the config file; CECManager forwards
     * the adapter and throttler sub-structs into their respective components
     * at construction and does no config I/O itself.
     */
    struct Options {
        bool scanDevicesAtStartup = false;
        CECAdapter::Options adapter;
        CommandThrottler::Options throttler;
    };

    /**
     * @param options     Fully-populated configuration options.
     * @param threadPool  Pool for background tasks (async adapter restart,
     *                    TV-standby suspend dispatch). Must outlive @c this.
     */
    CECManager(Options options, std::shared_ptr<ThreadPool> threadPool);
    ~CECManager();

    CECManager(const CECManager&) = delete;
    CECManager& operator=(const CECManager&) = delete;

    bool initialize();
    void shutdown();

    /** Close any open connection and re-detect/re-open the adapter. */
    bool reconnect();

    void setConnectionLostCallback(std::function<void()> callback);
    void setSuspendCallback(std::function<bool()> callback);

    /**
     * Invoked when the manager concludes the adapter is unrecoverable.
     * Runs on whichever thread surfaced the failure, so the callback must be
     * cheap and reentrant — no shutdown work inside.
     */
    void setFatalErrorCallback(std::function<void()> callback);

    /** Synchronous command dispatch. Thread-safe. */
    Message processCommand(const Message& command);

    bool isAdapterValid() const;

    /** Log a snapshot of connected devices. Caller must hold the outer lock. */
    void scanDevices();

    /** Fire standby/power-on at the configured device masks. */
    bool standbyDevices();
    bool powerOnDevices();

private:
    CECAdapter m_adapter;
    CommandThrottler m_throttler;

    Options m_options;
    std::shared_ptr<ThreadPool> m_threadPool;

    // Sole lock protecting adapter access and lifecycle transitions.
    mutable std::mutex m_managerMutex;

    int m_reconnectFailures = 0;

    std::function<bool()> m_suspendCallback;
    std::function<void()> m_fatalErrorCallback;
};

} // namespace cec_control
