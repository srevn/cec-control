#include "command_dispatcher.h"

#include <string_view>
#include <utility>

#include "../common/command_registry.h"
#include "../common/logger.h"
#include "../common/main_thread_work.h"
#include "adapter_lifecycle.h"
#include "app_config.h"
#include "cec/adapter_interface.h"
#include "cec/adapter_worker.h"
#include "command_dispatch.h"

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
      m_queueCommandsDuringSuspend(config.dispatcher.queueCommandsDuringSuspend),
      m_autoStandbyEnabled(config.dispatcher.autoStandbyEnabled),
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
    // once. DispatchClass::AdapterCall commands hand the sink to a
    // worker job that posts the invocation back through MainThreadWork;
    // every other branch replies synchronously on the main thread.
    if (!reply) return;  // defensive: a caller without a sink is malformed

    if (m_shutdownComplete) {
        reply(Message(MessageType::RESP_ERROR));
        return;
    }

    const DispatchSpec* spec = findDispatchByType(command.type);
    if (spec == nullptr) {
        // validateDispatchTable at startup ensures every kCommands
        // type has a row here, so the only way in is a response code
        // on the wire (client protocol violation) or an unregistered
        // new CMD_*. Either way, reject.
        LOG_ERROR("Unknown command type at dispatcher: ",
                  static_cast<int>(command.type));
        reply(Message(MessageType::RESP_ERROR));
        return;
    }

    if (spec->dispatch == DispatchClass::SupervisorIntercepted) {
        // CECDaemon::handleCommand short-circuits these before the
        // dispatcher sees them; reaching this branch means that
        // intercept is broken. Fail loudly rather than silently
        // routing to the adapter path.
        LOG_ERROR("SupervisorIntercepted command reached dispatcher: type=",
                  static_cast<int>(command.type));
        reply(Message(MessageType::RESP_ERROR));
        return;
    }

    if (m_lifecycle.isSuspended()) {
        reply(handleSuspendedInline(command, *spec));
        return;
    }

    switch (spec->dispatch) {
    case DispatchClass::StateOnly:
        // Only CMD_AUTO_STANDBY today.
        reply(applyAutoStandbyInline(command));
        return;
    case DispatchClass::AdapterCall:
        submitAdapterWork(*spec, std::move(command), std::move(reply));
        return;
    case DispatchClass::SupervisorIntercepted:
        // Handled above; listed here so -Wswitch stays honest over
        // the enumerator.
        break;
    }

    // Unreachable if validateDispatchTable passed at startup.
    LOG_ERROR("Unhandled DispatchClass for type=",
              static_cast<int>(command.type));
    reply(Message(MessageType::RESP_ERROR));
}

void CommandDispatcher::replay(std::vector<Message> commands) {
    if (commands.empty()) return;
    LOG_INFO("Processing ", commands.size(), " queued commands");
    for (auto& command : commands) {
        const DispatchSpec* spec = findDispatchByType(command.type);
        if (spec == nullptr || spec->dispatch != DispatchClass::AdapterCall) {
            // Only AdapterCall rows carry queueableWhileSuspended=true
            // in today's table, so non-AdapterCall should never have
            // been queued. Defensive log; skip without submitting.
            LOG_ERROR("Replay: unexpected non-AdapterCall type=",
                      static_cast<int>(command.type));
            continue;
        }
        m_worker.submit([this, command = std::move(command), spec]
                        (ICecAdapter& adapter) mutable {
            (void)executeOnAdapter(adapter, command, *spec);
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

Message CommandDispatcher::handleSuspendedInline(const Message& command,
                                                  const DispatchSpec& spec) {
    // Resolve the command's human-readable name from the client-side
    // registry for the log lines below. Two linear scans (one here
    // via findByType, one earlier via findDispatchByType) cost ~20 ns
    // over the registry's dozen entries — worth paying for operator-
    // legible diagnostics over a bare type integer.
    const CommandSpec* nameSpec = findByType(command.type);
    const std::string_view name =
        nameSpec != nullptr ? nameSpec->name
                            : std::string_view("unknown");

    if (m_queueCommandsDuringSuspend && spec.queueableWhileSuspended) {
        // We observed isSuspended() true a moment ago on the same
        // (main) thread, so the append is guaranteed; enqueue repeats
        // the check internally as a belt-and-braces safety net for
        // future callers.
        m_lifecycle.enqueue(command);
        LOG_INFO("Queued command '", name,
                 "' for execution after resume");
        return Message(MessageType::RESP_SUCCESS);
    }
    LOG_WARNING("Command '", name,
                "' received while suspended and cannot be queued");
    return Message(MessageType::RESP_ERROR);
}

void CommandDispatcher::submitAdapterWork(const DispatchSpec& spec,
                                           Message command,
                                           ResponseSink reply) {
    // DispatchSpec rows live in kDispatchTable's static storage, so
    // capturing a raw pointer to @p spec is safe across the worker-
    // then-main hop below.
    m_worker.submit([this, command = std::move(command),
                     reply = std::move(reply),
                     specPtr = &spec](ICecAdapter& adapter) mutable {
        Message response = executeOnAdapter(adapter, command, *specPtr);
        m_work.post([reply = std::move(reply),
                     response = std::move(response)]() mutable {
            reply(std::move(response));
        });
    });
}

Message CommandDispatcher::executeOnAdapter(ICecAdapter& adapter,
                                             const Message& command,
                                             const DispatchSpec& spec) {
    // Runs on the worker thread. The outer try/catch owns the
    // RESP_ERROR fallback; handlers are free to propagate exceptions
    // from ops::* or libcec.
    try {
        if (spec.requiresAdapterConnection && !adapter.isConnected()) {
            LOG_ERROR("Cannot process command: CEC adapter not connected");
            return Message(MessageType::RESP_ERROR);
        }
        if (spec.adapterHandler != nullptr &&
            spec.adapterHandler(adapter, m_throttler, command)) {
            return Message(MessageType::RESP_SUCCESS);
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during dispatch: ", e.what());
    }
    return Message(MessageType::RESP_ERROR);
}

} // namespace cec_control
