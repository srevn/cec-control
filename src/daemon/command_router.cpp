#include "command_router.h"

#include <chrono>
#include <utility>

#include "../common/command_registry.h"
#include "../common/logger.h"
#include "../common/main_thread_work.h"
#include "app_config.h"
#include "cec/adapter_interface.h"
#include "cec/adapter_worker.h"
#include "cec/operations.h"

namespace cec_control {

CommandRouter::CommandRouter(const AppConfig& config,
                             AdapterWorker&   worker,
                             MainThreadWork&  work,
                             Callbacks        callbacks)
    : m_worker(worker),
      m_work(work),
      m_throttler(config.throttler),
      m_queueCommandsDuringSuspend(config.queueCommandsDuringSuspend),
      m_autoStandbyEnabled(config.autoStandbyEnabled),
      m_suspendCallback(std::move(callbacks.onSuspendRequested)) {}

void CommandRouter::shutdown() {
    if (m_shutdownComplete) return;
    m_shutdownComplete = true;

    auto toDiscard = m_suspendQueue.drain();

    LOG_INFO("Shutting down CEC command router");
    if (!toDiscard.empty()) {
        LOG_INFO("Discarding ", toDiscard.size(),
                 " queued commands on shutdown");
    }
}

void CommandRouter::dispatch(Message command, ResponseSink reply) {
    // Every branch below is responsible for invoking @p reply exactly
    // once. Adapter-driven commands hand the sink to a worker job that
    // posts the invocation back through MainThreadWork; every other
    // branch replies synchronously on the main thread.
    if (!reply) return;  // defensive: a caller without a sink is malformed

    if (m_shutdownComplete) {
        reply(Message(MessageType::RESP_ERROR));
        return;
    }

    if (command.type == MessageType::CMD_RESTART_ADAPTER) {
        if (m_suspendQueue.isSuspended()) {
            LOG_WARNING("CMD_RESTART_ADAPTER rejected: router is suspended");
            reply(Message(MessageType::RESP_ERROR));
            return;
        }
        LOG_INFO("Scheduling adapter restart");
        m_worker.submit([](ICecAdapter& adapter) {
            if (adapter.reopenConnection()) {
                LOG_INFO("Adapter restart completed successfully");
            } else {
                LOG_ERROR("Failed to restart adapter");
            }
        });
        reply(Message(MessageType::RESP_SUCCESS));
        return;
    }

    if (m_suspendQueue.isSuspended()) {
        reply(handleSuspendedInline(command));
        return;
    }

    if (command.type == MessageType::CMD_AUTO_STANDBY) {
        reply(applyAutoStandbyInline(command));
        return;
    }

    // Adapter-driven. Submit and hop back through m_work so the sink
    // is always invoked on the main thread.
    m_worker.submit([this, command = std::move(command),
                     reply = std::move(reply)](ICecAdapter& adapter) mutable {
        Message response(MessageType::RESP_ERROR);
        try {
            if (adapter.isConnected()) {
                response = executeOnAdapter(adapter, command);
            } else {
                LOG_ERROR("Cannot process command: CEC adapter not connected");
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Exception during dispatch: ", e.what());
        }
        m_work.post([reply = std::move(reply),
                     response = std::move(response)]() mutable {
            reply(std::move(response));
        });
    });
}

void CommandRouter::suspendAsync(std::function<void(std::chrono::milliseconds)> onDone) {
    // Main-thread phase-1: flip the flag so dispatches arriving during
    // the worker-side close enter handleSuspendedInline.
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

void CommandRouter::resumeAsync(std::function<void(bool)> onDone) {
    if (m_shutdownComplete) {
        LOG_DEBUG("resume() called after shutdown; ignoring");
        if (onDone) onDone(false);
        return;
    }
    if (!m_suspendQueue.isSuspended()) {
        LOG_DEBUG("resume() called while not suspended");
        if (onDone) onDone(false);
        return;
    }

    LOG_INFO("Reinitializing CEC adapter after resume");
    m_worker.submit([this, onDone = std::move(onDone)](ICecAdapter& adapter) mutable {
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

void CommandRouter::reconnectAsync(std::function<void(bool)> onDone) {
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
    m_worker.submit([this, onDone = std::move(onDone)](ICecAdapter& adapter) mutable {
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

bool CommandRouter::isSuspended() const noexcept {
    return m_suspendQueue.isSuspended();
}

void CommandRouter::onTvStandby() {
    // Runs on libcec's command thread. m_autoStandbyEnabled is atomic;
    // m_suspendCallback is install-once at construction and never
    // reassigned, so calling it without a lock is safe.
    if (!m_autoStandbyEnabled.load(std::memory_order_acquire)) {
        LOG_DEBUG("TV standby observed; auto-standby disabled - ignoring");
        return;
    }
    if (!m_suspendCallback) {
        LOG_WARNING("TV standby observed but no suspend callback wired");
        return;
    }
    LOG_INFO("TV standby observed with auto-standby enabled; "
             "initiating system suspend");
    m_suspendCallback();
}

Message CommandRouter::applyAutoStandbyInline(const Message& command) {
    if (command.data.empty()) {
        return Message(MessageType::RESP_ERROR);
    }
    const bool enabled = command.data[0] > 0;
    m_autoStandbyEnabled.store(enabled, std::memory_order_release);
    LOG_INFO("Auto-standby ", enabled ? "enabled" : "disabled");
    return Message(MessageType::RESP_SUCCESS);
}

Message CommandRouter::handleSuspendedInline(const Message& command) {
    const auto* spec = findByType(command.type);
    if (m_queueCommandsDuringSuspend &&
        spec && spec->queueableWhileSuspended) {
        // We observed isSuspended() true a moment ago on the same
        // (main) thread, so the append is guaranteed; push() repeats
        // the check internally as a belt-and-braces safety net for
        // future callers.
        m_suspendQueue.push(command);
        LOG_INFO("Queued command type=", static_cast<int>(command.type),
                 " for execution after resume");
        return Message(MessageType::RESP_SUCCESS);
    }
    LOG_WARNING("Command type=", static_cast<int>(command.type),
                " received while suspended and cannot be queued");
    return Message(MessageType::RESP_ERROR);
}

Message CommandRouter::executeOnAdapter(ICecAdapter& adapter,
                                         const Message& command) {
    bool success = false;
    switch (command.type) {
    case MessageType::CMD_VOLUME_UP:
        success = ops::setVolume(adapter, m_throttler, command.deviceId, true);
        break;
    case MessageType::CMD_VOLUME_DOWN:
        success = ops::setVolume(adapter, m_throttler, command.deviceId, false);
        break;
    case MessageType::CMD_VOLUME_MUTE:
        success = ops::setMute(adapter, m_throttler, command.deviceId, true);
        break;
    case MessageType::CMD_POWER_ON:
        success = ops::powerOnDevice(adapter, m_throttler, command.deviceId);
        break;
    case MessageType::CMD_POWER_OFF:
        success = ops::powerOffDevice(adapter, m_throttler, command.deviceId);
        break;
    case MessageType::CMD_CHANGE_SOURCE:
        if (!command.data.empty()) {
            success = ops::setSource(adapter, m_throttler, command.data[0]);
        }
        break;
    default:
        LOG_ERROR("Unknown command type: ", static_cast<int>(command.type));
        return Message(MessageType::RESP_ERROR);
    }

    return success ? Message(MessageType::RESP_SUCCESS)
                   : Message(MessageType::RESP_ERROR);
}

void CommandRouter::onResumeWorkerComplete(
        bool adapterValid,
        std::function<void(bool)> onDone) {
    // Drain before flipping: any dispatch arriving on a later loop
    // iteration (after exitSuspended) is free to fall through to the
    // worker queue, racing these replays only at adapter-call
    // granularity — the exact property the pre-refactor code also had.
    std::vector<Message> drained = m_suspendQueue.drain();
    m_suspendQueue.exitSuspended();

    if (!adapterValid) {
        // Queued commands were accepted with RESP_SUCCESS ("accepted
        // for post-resume"). We cannot deliver on that promise; drop
        // and log.
        if (!drained.empty()) {
            LOG_WARNING("Discarding ", drained.size(),
                        " queued commands: reconnect failed");
        }
        if (onDone) onDone(false);
        return;
    }

    if (!drained.empty()) {
        LOG_INFO("Processing ", drained.size(), " queued commands");
        for (auto& command : drained) {
            m_worker.submit([this, command = std::move(command)]
                            (ICecAdapter& adapter) mutable {
                try {
                    (void)executeOnAdapter(adapter, command);
                } catch (const std::exception& e) {
                    LOG_ERROR("Exception processing queued command: ", e.what());
                }
            });
        }
    }

    if (onDone) onDone(true);
}

} // namespace cec_control
