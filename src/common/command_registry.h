#pragma once

#include "messages.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cec_control {

/**
 * Single source of truth for every client-facing command.
 *
 * The table is consumed by the argument parser (text → Message), the help
 * printer (rendering the client command list), and the wire-protocol filter
 * (does this MessageType correspond to a known command?). Adding a new client
 * command means appending one CommandSpec — nowhere else needs to learn about
 * it.
 *
 * Daemon-side dispatch deliberately lives in CommandDispatcher, not here.
 * Putting a handler pointer into the registry would force every TU that
 * includes this header to forward-declare a daemon class, blurring the
 * client/daemon boundary for no benefit: the daemon already owns one switch
 * on MessageType and that is the simplest possible authority.
 */
struct CommandSpec {
    /**
     * Per-command client argument parser. @p args contains the positional
     * arguments after the command name (e.g. for `cec-control power on 0`,
     * args is {"on", "0"}). Returns the parsed Message on success or
     * std::nullopt with a human-readable error written into @p err.
     *
     * Parsers own arity validation; the registry layer does not pre-check
     * args.size().
     */
    using ParseFn = std::optional<Message> (*)(const std::vector<std::string_view>& args,
                                                std::string& err);

    MessageType      type;

    /**
     * Type family covered by this spec (volume: UP/DOWN/MUTE;
     * power: ON/OFF; single-element otherwise). INVARIANT: `type`
     * must appear in `types`.
     *
     * Backing-array lifetime is tied to this initializer_list
     * subobject ([dcl.init.list]/6), which is safe only when the
     * enclosing CommandSpec has static storage duration (as with
     * kCommands). Do not construct CommandSpec values with a
     * runtime-built `types` — the member would dangle.
     */
    std::initializer_list<MessageType> types;

    std::string_view name;        // canonical command name (e.g. "power")
    std::string_view argSyntax;   // help-text syntax (e.g. "(on|off) DEVICE_ID")
    std::string_view help;        // one-line description
    ParseFn          parse;       // nonnull

    /**
     * True if this command is safe to park in the router's suspend queue for
     * execution after resume. Commands that mutate router policy (e.g.
     * auto-standby) or that have no meaningful post-resume effect (e.g.
     * source change while the TV is off) are marked false.
     *
     * Moot for the two daemon-intercepted lifecycle messages (CMD_SUSPEND,
     * CMD_RESUME) — they never reach the router's queueable check — and for
     * CMD_RESTART_ADAPTER, which is dispatched asynchronously before the
     * suspend branch runs. All three keep the field at false for clarity.
     */
    bool             queueableWhileSuspended;
};

/**
 * Authoritative command table. Order is the order help text renders in, and
 * is also the order new readers will encounter. Keep related commands grouped.
 */
extern const std::array<CommandSpec, 7> kCommands;

/** Linear lookup by canonical name. Returns nullptr if no match. */
const CommandSpec* findByName(std::string_view name) noexcept;

/** Linear lookup by MessageType. Returns nullptr if @p type is not a command. */
const CommandSpec* findByType(MessageType type) noexcept;

} // namespace cec_control
