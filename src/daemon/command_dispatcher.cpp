#include "command_dispatcher.h"

#include <utility>

#include "../common/command_registry.h"
#include "../common/logger.h"
#include "../common/main_thread_work.h"
#include "adapter_lifecycle.h"
#include "app_config.h"
#include "cec/adapter_interface.h"
#include "cec/adapter_worker.h"
#include "cec/operations.h"

namespace cec_control {

CommandDispatcher::CommandDispatcher(const AppConfig&  config,
                                     AdapterWorker&    worker,
                                     MainThreadWork&   work,
                                     AdapterLifecycle& lifecycle,
                                     Callbacks         callbacks)
    : m_worker(worker),
      m_work(work),
      m_lifecycle(lifecycle),
      m_throttler(config.throttler),
      m_queueCommandsDuringSuspend(config.router.queueCommandsDuringSuspend),
      m_autoStandbyEnabled(config.router.autoStandbyEnabled),
      m_suspendCallback(std::move(callbacks.onSuspendRequested)) {}

void CommandDispatcher::shutdown() {
    if (m_shutdownComplete) return;
    m_shutdownComplete = true;
    LOG_INFO("Shutting down command dispatcher");
}

bool CommandDispatcher::isShutdown() const noexcept {
    return m_shutdownComplete;
}

void CommandDispatcher::dispatch(Message command, ResponseSink reply) {
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
        if (m_lifecycle.isSuspended()) {
            LOG_WARNING("CMD_RESTART_ADAPTER rejected: lifecycle is suspended");
            reply(Message(MessageType::RESP_ERROR));
            return;
        }
        LOG_INFO("Scheduling adapter restart");
        // Mirror the adapter-driven pattern below: submit a worker job
        // that posts the real RESP_SUCCESS / RESP_ERROR back through
        // m_work once reopenConnection() has actually completed. The
        // previous fire-and-forget shape replied RESP_SUCCESS before
        // the worker ran, masking genuine reopen failures.
        m_worker.submit([this, reply = std::move(reply)]
                        (ICecAdapter& adapter) mutable {
            Message response(MessageType::RESP_ERROR);
            try {
                if (adapter.reopenConnection()) {
                    LOG_INFO("Adapter restart completed successfully");
                    response = Message(MessageType::RESP_SUCCESS);
                } else {
                    LOG_ERROR("Failed to restart adapter");
                }
            } catch (const std::exception& e) {
                LOG_ERROR("Exception during adapter restart: ", e.what());
            }
            m_work.post([reply = std::move(reply),
                         response = std::move(response)]() mutable {
                reply(std::move(response));
            });
        });
        return;
    }

    if (m_lifecycle.isSuspended()) {
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

void CommandDispatcher::replay(std::vector<Message> commands) {
    if (commands.empty()) return;
    LOG_INFO("Processing ", commands.size(), " queued commands");
    for (auto& command : commands) {
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

void CommandDispatcher::onTvStandby() {
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

Message CommandDispatcher::applyAutoStandbyInline(const Message& command) {
    if (command.data.empty()) {
        return Message(MessageType::RESP_ERROR);
    }
    const bool enabled = command.data[0] > 0;
    m_autoStandbyEnabled.store(enabled, std::memory_order_release);
    LOG_INFO("Auto-standby ", enabled ? "enabled" : "disabled");
    return Message(MessageType::RESP_SUCCESS);
}

Message CommandDispatcher::handleSuspendedInline(const Message& command) {
    const auto* spec = findByType(command.type);
    if (m_queueCommandsDuringSuspend &&
        spec && spec->queueableWhileSuspended) {
        // We observed isSuspended() true a moment ago on the same
        // (main) thread, so the append is guaranteed; enqueue repeats
        // the check internally as a belt-and-braces safety net for
        // future callers.
        m_lifecycle.enqueue(command);
        LOG_INFO("Queued command type=", static_cast<int>(command.type),
                 " for execution after resume");
        return Message(MessageType::RESP_SUCCESS);
    }
    LOG_WARNING("Command type=", static_cast<int>(command.type),
                " received while suspended and cannot be queued");
    return Message(MessageType::RESP_ERROR);
}

Message CommandDispatcher::executeOnAdapter(ICecAdapter& adapter,
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
        if (command.data.empty()) {
            // The registry's parser guarantees a single-byte payload; an
            // empty data vector here means a hand-rolled wire message
            // bypassed the parser. Log so it is distinguishable from a
            // CEC-layer failure, then surface RESP_ERROR below.
            LOG_WARNING("CMD_CHANGE_SOURCE received with empty payload; "
                        "expected source byte in data[0] (malformed client)");
            break;
        }
        success = ops::setSource(adapter, m_throttler, command.data[0]);
        break;
    default:
        LOG_ERROR("Unknown command type: ", static_cast<int>(command.type));
        return Message(MessageType::RESP_ERROR);
    }

    return success ? Message(MessageType::RESP_SUCCESS)
                   : Message(MessageType::RESP_ERROR);
}

} // namespace cec_control
