#include "command_dispatch.h"

#include <array>
#include <cstddef>

#include "../common/command_registry.h"
#include "../common/logger.h"
#include "cec/adapter_interface.h"
#include "cec/operations.h"
#include "command_throttler.h"

namespace cec_control {

namespace {

// AdapterCall handlers: uniform shape adapting the ops::* entry points
// to the single AdapterCallHandler signature. Keeping the wrappers
// next to the table means reading this TU gives a complete picture of
// daemon-side dispatch.
//
// Exception policy: the wrappers do not swallow exceptions; the worker
// submission wrapper (CommandDispatcher::submitAdapterWork) owns the
// outer try/catch and the RESP_ERROR fallback, so propagating through
// these handlers is intentional.

bool handlePowerOn(ICecAdapter& adapter, CommandThrottler& throttler,
                   const Message& command) {
    return ops::powerOnDevice(adapter, throttler, command.deviceId);
}

bool handlePowerOff(ICecAdapter& adapter, CommandThrottler& throttler,
                    const Message& command) {
    return ops::powerOffDevice(adapter, throttler, command.deviceId);
}

bool handleVolumeUp(ICecAdapter& adapter, CommandThrottler& throttler,
                    const Message& command) {
    return ops::setVolume(adapter, throttler, command.deviceId, /*up=*/true);
}

bool handleVolumeDown(ICecAdapter& adapter, CommandThrottler& throttler,
                      const Message& command) {
    return ops::setVolume(adapter, throttler, command.deviceId, /*up=*/false);
}

bool handleVolumeMute(ICecAdapter& adapter, CommandThrottler& throttler,
                      const Message& command) {
    return ops::setMute(adapter, throttler, command.deviceId, /*mute=*/true);
}

bool handleChangeSource(ICecAdapter& adapter, CommandThrottler& throttler,
                        const Message& command) {
    if (command.data.empty()) {
        // The registry's parser guarantees a single-byte payload; an
        // empty data vector here means a hand-rolled wire message
        // bypassed the parser. Log so it is distinguishable from a
        // CEC-layer failure, then surface the failure via the wrapper.
        LOG_WARNING("CMD_CHANGE_SOURCE received with empty payload; "
                    "expected source byte in data[0] (malformed client)");
        return false;
    }
    return ops::setSource(adapter, throttler, command.data[0]);
}

bool handleRestartAdapter(ICecAdapter& adapter, CommandThrottler& /*throttler*/,
                          const Message& /*command*/) {
    // CMD_RESTART_ADAPTER bypasses the isConnected() gate (see the
    // corresponding kDispatchTable row) because reopening a
    // disconnected adapter is the explicit intent. Throttling does not
    // apply — this is an operator-triggered recovery path, not a
    // throttled CEC command.
    return adapter.reopenConnection();
}

// Source of truth for daemon-side command handling. C++17 aggregate
// deduction for std::array avoids a manual size literal: adding an
// entry does not require updating a size constant anywhere.
//
// INVARIANTS enforced by validateDispatchTable() at startup:
//   - No two rows share the same MessageType.
//   - Every MessageType reachable via kCommands.types has a row here.
//   - Every row in this table has a matching kCommands entry.
//   - adapterHandler is non-null iff dispatch == AdapterCall.
constexpr std::array kDispatchTable = {
    DispatchSpec{MessageType::CMD_POWER_ON,
                 DispatchClass::AdapterCall,
                 /*queueableWhileSuspended=*/true,
                 /*requiresAdapterConnection=*/true,
                 handlePowerOn},
    DispatchSpec{MessageType::CMD_POWER_OFF,
                 DispatchClass::AdapterCall,
                 true, true, handlePowerOff},
    DispatchSpec{MessageType::CMD_VOLUME_UP,
                 DispatchClass::AdapterCall,
                 true, true, handleVolumeUp},
    DispatchSpec{MessageType::CMD_VOLUME_DOWN,
                 DispatchClass::AdapterCall,
                 true, true, handleVolumeDown},
    DispatchSpec{MessageType::CMD_VOLUME_MUTE,
                 DispatchClass::AdapterCall,
                 true, true, handleVolumeMute},
    DispatchSpec{MessageType::CMD_CHANGE_SOURCE,
                 DispatchClass::AdapterCall,
                 /*queueableWhileSuspended=*/false,
                 /*requiresAdapterConnection=*/true,
                 handleChangeSource},
    DispatchSpec{MessageType::CMD_RESTART_ADAPTER,
                 DispatchClass::AdapterCall,
                 /*queueableWhileSuspended=*/false,
                 /*requiresAdapterConnection=*/false,
                 handleRestartAdapter},
    DispatchSpec{MessageType::CMD_AUTO_STANDBY,
                 DispatchClass::StateOnly,
                 false, false, nullptr},
    DispatchSpec{MessageType::CMD_SUSPEND,
                 DispatchClass::SupervisorIntercepted,
                 false, false, nullptr},
    DispatchSpec{MessageType::CMD_RESUME,
                 DispatchClass::SupervisorIntercepted,
                 false, false, nullptr},
};

} // namespace

const DispatchSpec* findDispatchByType(MessageType type) noexcept {
    for (const auto& spec : kDispatchTable) {
        if (spec.type == type) return &spec;
    }
    return nullptr;
}

bool validateDispatchTable() {
    bool ok = true;

    // Check 1: every MessageType reachable through kCommands.types has
    // a kDispatchTable row. Catches the "added a new command to the
    // client registry but forgot the daemon-side entry" failure mode.
    for (const auto& cmd : kCommands) {
        for (MessageType type : cmd.types) {
            if (findDispatchByType(type) == nullptr) {
                LOG_ERROR("Dispatch table missing entry for type=",
                          static_cast<int>(type),
                          " (kCommands: '", cmd.name, "')");
                ok = false;
            }
        }
    }

    // Check 2: every kDispatchTable row has a matching kCommands
    // entry. Catches "added a daemon-side entry for a MessageType that
    // nobody can parse from the wire".
    for (const auto& spec : kDispatchTable) {
        if (findByType(spec.type) == nullptr) {
            LOG_ERROR("kDispatchTable entry for type=",
                      static_cast<int>(spec.type),
                      " has no matching kCommands entry");
            ok = false;
        }
    }

    // Check 3: no duplicate MessageType in kDispatchTable. Without
    // this, findDispatchByType silently returns the first match and
    // the second entry is dead; the programmer error goes unnoticed.
    for (std::size_t i = 0; i < kDispatchTable.size(); ++i) {
        for (std::size_t j = i + 1; j < kDispatchTable.size(); ++j) {
            if (kDispatchTable[i].type == kDispatchTable[j].type) {
                LOG_ERROR("kDispatchTable has duplicate entries for type=",
                          static_cast<int>(kDispatchTable[i].type));
                ok = false;
            }
        }
    }

    // Check 4: adapterHandler is non-null iff dispatch == AdapterCall.
    // The dispatcher's worker path assumes it can call the handler
    // unconditionally on an AdapterCall row; a null handler there
    // would crash the worker thread.
    for (const auto& spec : kDispatchTable) {
        const bool isAdapterCall =
            spec.dispatch == DispatchClass::AdapterCall;
        const bool hasHandler = spec.adapterHandler != nullptr;
        if (isAdapterCall != hasHandler) {
            LOG_ERROR("kDispatchTable entry type=",
                      static_cast<int>(spec.type),
                      " violates handler/class invariant (AdapterCall=",
                      isAdapterCall, ", handler=",
                      hasHandler, ")");
            ok = false;
        }
    }

    if (ok) {
        LOG_DEBUG("Dispatch table validated: ", kDispatchTable.size(),
                  " entries cross-checked against kCommands");
    }
    return ok;
}

} // namespace cec_control
