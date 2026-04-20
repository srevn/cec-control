#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace cec_control {

/**
 * Cross-thread work queue that lands on the main event loop.
 *
 * Any thread may post() a closure; the main thread drains the queue by
 * reading the wake fd and invoking drain(). The wake fd is a non-blocking
 * eventfd — register it with the EventLoop for READ and call drain() from
 * its handler.
 *
 * This is the preferred mechanism for any subsystem thread that needs to
 * influence main-loop state: libCEC callbacks (connection lost, TV
 * standby), thread-pool workers wanting to perform sd-bus calls, and so
 * on. Keeps the main loop the sole owner of sd-bus, signal, and other
 * non-reentrant state without scattering atomic flags across the daemon.
 */
class MainThreadWork {
public:
    MainThreadWork();
    ~MainThreadWork();

    MainThreadWork(const MainThreadWork&) = delete;
    MainThreadWork& operator=(const MainThreadWork&) = delete;

    /** True if the underlying eventfd was created successfully. */
    [[nodiscard]] bool valid() const noexcept { return m_wakeFd >= 0; }

    /** The fd to register with EventLoop (READ). */
    int fd() const noexcept { return m_wakeFd; }

    /**
     * Enqueue @p work and wake the main loop. Safe to call from any
     * thread, including signal-adjacent contexts with the caveat that
     * allocating a std::function is not async-signal-safe.
     */
    void post(std::function<void()> work);

    /**
     * Drain the queue and run every pending closure in FIFO order. Must
     * only be called on the main thread. Closures that post() more work
     * during their run are picked up on the next drain call, not inline.
     */
    void drain();

private:
    int m_wakeFd = -1;
    std::mutex m_mutex;
    std::vector<std::function<void()>> m_pending;
};

} // namespace cec_control
