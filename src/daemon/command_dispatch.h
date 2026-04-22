#pragma once

#include "../common/messages.h"

namespace cec_control {

class CommandThrottler;
class ICecAdapter;

/**
 * @brief Daemon-side dispatch class for a wire command.
 *
 * Every @c CMD_* message maps to exactly one @c DispatchClass via the
 * @c kDispatchTable entry keyed on its @c MessageType. The dispatcher
 * (@c CommandDispatcher::dispatch) and the daemon's top-level
 * @c handleCommand each consult the table once per incoming command to
 * decide where the message is handled; the classes are named for that
 * destination rather than for what the command "does".
 *
 *  - @c SupervisorIntercepted: the daemon short-circuits before the
 *    dispatcher is ever invoked and feeds @c PowerSupervisor directly.
 *    Applies to @c CMD_SUSPEND and @c CMD_RESUME.
 *  - @c StateOnly: the dispatcher mutates its own main-thread state
 *    (no adapter touch, no worker hop) and replies inline. Applies to
 *    @c CMD_AUTO_STANDBY.
 *  - @c AdapterCall: the dispatcher submits a worker job that invokes
 *    the spec's @c adapterHandler and posts the reply back via
 *    @c MainThreadWork. Includes every command that ultimately drives
 *    libcec, plus @c CMD_RESTART_ADAPTER (which runs a reopen without
 *    the normal @c isConnected() gate — see @c requiresAdapterConnection).
 */
enum class DispatchClass {
    SupervisorIntercepted,
    StateOnly,
    AdapterCall,
};

/**
 * Handler signature for @c DispatchClass::AdapterCall entries.
 *
 * Called on the @c AdapterWorker thread under the dispatcher's
 * @c submitAdapterWork wrapper: the wrapper owns the per-call
 * @c isConnected() gate (controlled by @c requiresAdapterConnection)
 * and the enclosing try/catch, so handlers are free to propagate
 * exceptions from @c ops or libcec. The handler returns whether the
 * underlying throttled attempt ultimately succeeded.
 */
using AdapterCallHandler =
    bool (*)(ICecAdapter& adapter,
             CommandThrottler& throttler,
             const Message& command);

/**
 * @brief Table row describing the daemon-side handling of one wire
 *        command.
 *
 * Runtime-validated against @c kCommands at startup (see
 * @c validateDispatchTable). The table is deliberately kept on the
 * daemon side so the client-shared @c kCommands registry can stay
 * free of libcec-adjacent concerns.
 */
struct DispatchSpec {
    MessageType        type;
    DispatchClass      dispatch;

    /**
     * True if this command is safe to park in the lifecycle's
     * suspend queue for execution after resume. Consumed only from
     * @c CommandDispatcher::handleSuspendedInline; moot for classes
     * other than @c AdapterCall (which is why @c SupervisorIntercepted
     * and @c StateOnly rows keep it @c false).
     */
    bool               queueableWhileSuspended;

    /**
     * True if the worker-side dispatch path must observe
     * @c ICecAdapter::isConnected() before running @c adapterHandler.
     * @c false only for @c CMD_RESTART_ADAPTER, whose whole purpose is
     * to recover from a lost connection. Moot for non-@c AdapterCall
     * classes.
     */
    bool               requiresAdapterConnection;

    /**
     * Function pointer invoked from the worker for @c AdapterCall
     * entries. INVARIANT: non-null iff @c dispatch ==
     * @c DispatchClass::AdapterCall; @c validateDispatchTable enforces
     * the equivalence at startup.
     */
    AdapterCallHandler adapterHandler;
};

/**
 * Linear lookup by @c MessageType. Returns @c nullptr if the type has
 * no entry (which means either the type is a response code, or the
 * dispatch table is structurally broken — the validator catches the
 * latter at startup).
 */
[[nodiscard]] const DispatchSpec* findDispatchByType(MessageType type) noexcept;

/**
 * Cross-check the daemon-side dispatch table against the client-shared
 * @c kCommands registry plus internal invariants. Called from
 * @c CECDaemon::start before any subsystem that would consult the
 * table is constructed; returns @c false if any check fails, having
 * logged one @c LOG_ERROR per violation. A @c false return causes the
 * daemon to abort startup rather than run with silently-incorrect
 * dispatch.
 */
[[nodiscard]] bool validateDispatchTable();

} // namespace cec_control
