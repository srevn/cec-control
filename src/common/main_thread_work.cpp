#include "main_thread_work.h"
#include "logger.h"

#include <sys/eventfd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <utility>

namespace cec_control {

MainThreadWork::MainThreadWork() {
    m_wakeFd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m_wakeFd < 0) {
        LOG_ERROR("Failed to create MainThreadWork eventfd: ",
                  std::strerror(errno));
    }
}

MainThreadWork::~MainThreadWork() {
    if (m_wakeFd >= 0) {
        ::close(m_wakeFd);
        m_wakeFd = -1;
    }
}

void MainThreadWork::post(std::function<void()> work) {
    if (!work) return;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(std::move(work));
    }

    if (m_wakeFd < 0) return;
    const uint64_t one = 1;
    ssize_t r = ::write(m_wakeFd, &one, sizeof(one));
    (void)r;  // Best-effort wake; counter saturation is harmless.
}

void MainThreadWork::drain() {
    if (m_wakeFd >= 0) {
        // Clear the counter so epoll won't re-fire before new work arrives.
        uint64_t scratch;
        while (::read(m_wakeFd, &scratch, sizeof(scratch)) > 0) {
            // eventfd delivers the current sum and resets to zero.
        }
    }

    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        batch.swap(m_pending);
    }

    // Run outside the lock so closures that post() additional work don't
    // self-deadlock; the new items wait for the next drain().
    for (auto& work : batch) {
        try {
            work();
        } catch (const std::exception& e) {
            LOG_ERROR("MainThreadWork closure threw: ", e.what());
        } catch (...) {
            LOG_ERROR("MainThreadWork closure threw non-std exception");
        }
    }
}

} // namespace cec_control
