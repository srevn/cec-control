#include "adapter_worker.h"

#include "../../common/logger.h"

#include <pthread.h>

#include <utility>

namespace cec_control {

AdapterWorker::AdapterWorker(std::unique_ptr<ICecAdapter> adapter) noexcept
    : m_adapter(std::move(adapter)) {}

AdapterWorker::~AdapterWorker() {
    stop();
}

void AdapterWorker::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started) return;
    m_started = true;
    m_thread  = std::thread(&AdapterWorker::run, this);
}

void AdapterWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopRequested) return;
        m_stopRequested = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    // The worker thread has exited. It performed the adapter close as
    // its final action, so we are safe to release the unique_ptr from
    // the calling (main) thread. Doing so here rather than waiting for
    // destruction releases the CEC handle promptly.
    m_adapter.reset();
}

void AdapterWorker::submit(Job job) {
    if (!job) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopRequested) return;
        m_jobs.push(std::move(job));
    }
    m_cv.notify_one();
}

bool AdapterWorker::isAdapterConnected() const noexcept {
    return m_adapter && m_adapter->isConnected();
}

void AdapterWorker::run() {
    // Make the thread identifiable in `top -H`, `gdb thread apply all`,
    // etc. The name is silently truncated to 15 bytes by the kernel.
    ::pthread_setname_np(::pthread_self(), "cec-adapter");

    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_stopRequested || !m_jobs.empty();
            });
            if (m_stopRequested) {
                // Drop pending jobs on stop. Their completions (if any)
                // would land on a main-thread work queue no one is
                // draining — executing them would burn libcec time to
                // produce closures that are immediately destructed.
                std::queue<Job> dropped;
                dropped.swap(m_jobs);
                break;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop();
        }

        try {
            job(*m_adapter);
        } catch (const std::exception& e) {
            LOG_ERROR("AdapterWorker job threw: ", e.what());
        } catch (...) {
            LOG_ERROR("AdapterWorker job threw non-std exception");
        }
    }

    // Final action on the worker thread: close the adapter so libcec's
    // internal command and alert threads stop before the unique_ptr is
    // released from stop(). Exceptions here are logged but not
    // rethrown; we still need to exit the thread cleanly.
    if (m_adapter) {
        try {
            m_adapter->closeConnection();
        } catch (const std::exception& e) {
            LOG_ERROR("AdapterWorker close-on-exit threw: ", e.what());
        } catch (...) {
            LOG_ERROR("AdapterWorker close-on-exit threw non-std exception");
        }
    }
}

} // namespace cec_control
