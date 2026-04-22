#include "adapter_lifecycle.h"

#include <chrono>
#include <utility>

#include "../common/logger.h"
#include "../common/main_thread_work.h"
#include "cec/adapter_interface.h"
#include "cec/adapter_worker.h"

namespace cec_control {

AdapterLifecycle::AdapterLifecycle(AdapterWorker&  worker,
                                   MainThreadWork& work) noexcept
    : m_worker(worker),
      m_work(work) {}

void AdapterLifecycle::shutdown() {
    if (m_shutdownComplete) return;
    m_shutdownComplete = true;

    auto toDiscard = m_suspendQueue.drain();

    LOG_INFO("Shutting down adapter lifecycle");
    if (!toDiscard.empty()) {
        LOG_INFO("Discarding ", toDiscard.size(),
                 " queued commands on shutdown");
    }
}

bool AdapterLifecycle::isShutdown() const noexcept {
    return m_shutdownComplete;
}

bool AdapterLifecycle::isSuspended() const noexcept {
    return m_suspendQueue.isSuspended();
}

void AdapterLifecycle::enqueue(const Message& cmd) {
    m_suspendQueue.push(cmd);
}

void AdapterLifecycle::suspendAsync(
        std::function<void(std::chrono::milliseconds)> onDone) {
    // Main-thread phase-1: flip the flag so dispatches arriving during
    // the worker-side close enter the dispatcher's suspended-inline path.
    if (m_shutdownComplete) {
        LOG_DEBUG("suspend() called after shutdown; ignoring");
        if (onDone) onDone(std::chrono::milliseconds(0));
        return;
    }
    if (m_suspendQueue.isSuspended()) {
        LOG_DEBUG("suspend() called while already suspended");
        if (onDone) onDone(std::chrono::milliseconds(0));
        return;
    }
    m_suspendQueue.enterSuspended();

    LOG_INFO("Preparing CEC adapter for system sleep");
    const auto submittedAt = std::chrono::steady_clock::now();
    m_worker.submit([this, onDone = std::move(onDone),
                     submittedAt](ICecAdapter& adapter) mutable {
        if (adapter.isConnected()) {
            try {
                (void)adapter.standbyDevices(CEC::CECDEVICE_BROADCAST);
            } catch (const std::exception& e) {
                LOG_ERROR("Exception sending standby commands: ", e.what());
            }
        }
        adapter.closeConnection();
        LOG_INFO("CEC adapter closed for suspend");

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - submittedAt);
        m_work.post([onDone = std::move(onDone), elapsed]() mutable {
            if (onDone) onDone(elapsed);
        });
    });
}

void AdapterLifecycle::resumeAsync(ResumeCallback onDone) {
    if (m_shutdownComplete) {
        LOG_DEBUG("resume() called after shutdown; ignoring");
        if (onDone) onDone(false, {});
        return;
    }
    if (!m_suspendQueue.isSuspended()) {
        LOG_DEBUG("resume() called while not suspended");
        if (onDone) onDone(false, {});
        return;
    }

    LOG_INFO("Reinitializing CEC adapter after resume");
    m_worker.submit([this, onDone = std::move(onDone)]
                    (ICecAdapter& adapter) mutable {
        const bool reconnected = adapter.reopenConnection();
        if (reconnected) {
            LOG_INFO("CEC adapter reconnected successfully on resume");
            try {
                (void)adapter.powerOnDevices(CEC::CECDEVICE_BROADCAST);
            } catch (const std::exception& e) {
                LOG_ERROR("Exception sending power-on commands: ", e.what());
            }
        } else {
            LOG_ERROR("Failed to reconnect CEC adapter on resume");
        }
        // Re-read the connection hint after powerOnDevices; the
        // lifecycle FSM uses this to decide whether to arm the
        // post-resume retry timer, so prefer the adapter's own
        // up-to-date view over @c reconnected.
        const bool adapterValid = adapter.isConnected();
        m_work.post([this, onDone = std::move(onDone),
                     adapterValid]() mutable {
            onResumeWorkerComplete(adapterValid, std::move(onDone));
        });
    });
}

void AdapterLifecycle::reconnectAsync(std::function<void(bool)> onDone) {
    if (m_shutdownComplete) {
        LOG_DEBUG("reconnect() called after shutdown; ignoring");
        if (onDone) onDone(false);
        return;
    }
    if (m_suspendQueue.isSuspended()) {
        // The adapter is intentionally closed during suspend. A stray
        // alert arriving between suspend() and kernel sleep is the
        // typical trigger — drop it and wait for resume to reopen.
        LOG_DEBUG("reconnect() called while suspended; ignoring");
        if (onDone) onDone(false);
        return;
    }
    if (m_worker.isAdapterConnected()) {
        LOG_DEBUG("reconnect(): adapter already connected");
        if (onDone) onDone(true);
        return;
    }

    LOG_INFO("Attempting to reconnect to CEC adapter");
    m_worker.submit([this, onDone = std::move(onDone)]
                    (ICecAdapter& adapter) mutable {
        const bool ok = adapter.reopenConnection();
        if (ok) {
            LOG_INFO("CEC adapter reconnected successfully");
        } else {
            LOG_ERROR("Failed to reconnect CEC adapter");
        }
        m_work.post([onDone = std::move(onDone), ok]() mutable {
            if (onDone) onDone(ok);
        });
    });
}

void AdapterLifecycle::onResumeWorkerComplete(bool adapterValid,
                                              ResumeCallback onDone) {
    // Drain before flipping: any dispatch arriving on a later loop
    // iteration (after exitSuspended) is free to fall through to the
    // worker queue, racing the eventual replays only at adapter-call
    // granularity — the exact property the pre-refactor code had.
    std::vector<Message> drained = m_suspendQueue.drain();
    m_suspendQueue.exitSuspended();

    if (!adapterValid) {
        // Queued commands were accepted with RESP_SUCCESS ("accepted
        // for post-resume"). We cannot deliver on that promise; drop
        // and log here so the orchestrator never sees commands it
        // cannot replay.
        if (!drained.empty()) {
            LOG_WARNING("Discarding ", drained.size(),
                        " queued commands: reconnect failed");
        }
        if (onDone) onDone(false, {});
        return;
    }

    if (onDone) onDone(true, std::move(drained));
}

} // namespace cec_control
