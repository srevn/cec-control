#pragma once

#include "messages.h"

#include <string>
#include <variant>

namespace cec_control {

/** Selector for HelpPrinter; an ordinary enum, not a heuristic flag. */
enum class HelpTarget {
    General,
    Client,
    Daemon
};

/** Parsing failed; @c message is suitable for printing to stderr verbatim. */
struct ParseError {
    std::string message;
};

/** Show help and exit successfully. */
struct ShowHelp {
    HelpTarget target = HelpTarget::General;
};

/**
 * Run a single client command against the daemon and exit. @c command is the
 * fully-built wire message; @c socketPathOverride is empty when the caller
 * did not pass --socket-path= (the SocketClient then resolves the default
 * via SystemPaths).
 */
struct RunClient {
    Message     command;
    std::string socketPathOverride;
};

/**
 * Run the daemon with the given lifecycle options. Empty file paths mean
 * "use SystemPaths defaults"; the bootstrap layer materialises them.
 *
 * The daemon always runs in the foreground; backgrounding is delegated
 * to the supervisor (systemd, systemd-run, or the shell's @c & ), so
 * there is no option to control that here.
 */
struct RunDaemon {
    bool        verbose = false;
    std::string logFile;
    std::string configFile;
};

/**
 * Result of parsing argv. Exhaustive: every successful or unsuccessful path
 * lands in exactly one of these alternatives. main() dispatches via
 * std::visit, which makes the four control-flow branches an exhaustive
 * match the compiler can check.
 */
using Action = std::variant<ParseError, ShowHelp, RunClient, RunDaemon>;

class ArgumentParser {
public:
    /**
     * Parse argv[1..argc) and return the resulting Action. argv[0] is
     * ignored (the program name belongs to the caller, not the parser).
     */
    static Action parse(int argc, char* const argv[]);
};

} // namespace cec_control
