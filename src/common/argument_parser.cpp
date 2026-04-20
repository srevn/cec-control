#include "argument_parser.h"

#include "command_registry.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cec_control {

namespace {

constexpr std::string_view kSocketPathPrefix = "--socket-path=";

bool isHelpFlag(std::string_view arg) noexcept {
    return arg == "--help" || arg == "-h";
}

/** Map a `help <X>` token to the matching target; unknown tokens fall back to General. */
HelpTarget helpTargetFromToken(std::string_view token) noexcept {
    if (token == "client") return HelpTarget::Client;
    if (token == "daemon") return HelpTarget::Daemon;
    return HelpTarget::General;
}

/**
 * Parse the daemon's option set. The args vector is whatever follows the
 * `daemon` subcommand. Recognises --help/-h as a request to render daemon
 * help.
 */
Action parseDaemonOptions(const std::vector<std::string_view>& args) {
    RunDaemon out;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        if (arg == "--verbose" || arg == "-v") {
            out.verbose = true;
            continue;
        }
        if (arg == "--foreground" || arg == "-f") {
            out.foreground = true;
            continue;
        }
        if (arg == "--log" || arg == "-l") {
            if (i + 1 >= args.size()) {
                return ParseError{"Error: " + std::string(arg) + " requires a file path"};
            }
            const std::string_view value = args[++i];
            if (value.empty()) {
                return ParseError{"Error: empty log file path provided"};
            }
            out.logFile.assign(value);
            continue;
        }
        if (arg == "--config" || arg == "-c") {
            if (i + 1 >= args.size()) {
                return ParseError{"Error: " + std::string(arg) + " requires a file path"};
            }
            const std::string_view value = args[++i];
            if (value.empty()) {
                return ParseError{"Error: empty config file path provided"};
            }
            out.configFile.assign(value);
            continue;
        }
        if (isHelpFlag(arg)) {
            return ShowHelp{HelpTarget::Daemon};
        }
        if (!arg.empty() && arg.front() == '-') {
            return ParseError{"Error: unknown daemon option: " + std::string(arg)};
        }
        // Reject positional args explicitly: misuse like
        // `cec-control daemon power on 0` should fail loudly rather than be
        // silently absorbed as "unknown option".
        return ParseError{"Error: daemon takes no positional arguments (got '" +
                          std::string(arg) + "')"};
    }

    return out;
}

/**
 * Strip --socket-path=VALUE flags from @p args, populating @p socketPath. Any
 * occurrence with an empty value or a duplicate definition is a hard error;
 * remaining tokens are returned unchanged for the per-command parser.
 */
std::variant<std::vector<std::string_view>, ParseError>
extractClientFlags(const std::vector<std::string_view>& args,
                   std::string& socketPath) {
    std::vector<std::string_view> positional;
    positional.reserve(args.size());

    for (const std::string_view arg : args) {
        if (arg.size() >= kSocketPathPrefix.size() &&
            arg.substr(0, kSocketPathPrefix.size()) == kSocketPathPrefix) {
            const std::string_view value = arg.substr(kSocketPathPrefix.size());
            if (value.empty()) {
                return ParseError{"Error: --socket-path= requires a value"};
            }
            if (!socketPath.empty()) {
                return ParseError{"Error: --socket-path= specified multiple times"};
            }
            socketPath.assign(value);
            continue;
        }
        positional.push_back(arg);
    }
    return positional;
}

Action parseClientCommand(const CommandSpec& spec,
                           const std::vector<std::string_view>& argsAfterCommand) {
    std::string socketPath;
    auto extracted = extractClientFlags(argsAfterCommand, socketPath);
    if (auto* err = std::get_if<ParseError>(&extracted)) {
        return std::move(*err);
    }

    std::string err;
    auto cmd = spec.parse(std::get<std::vector<std::string_view>>(extracted), err);
    if (!cmd) {
        return ParseError{"Error: " + err};
    }
    return RunClient{std::move(*cmd), std::move(socketPath)};
}

/**
 * Build a string_view view of argv[1..argc) without copying. The lifetime is
 * argv's, which outlives the parse() return value (argv lives for the whole
 * process).
 */
std::vector<std::string_view> sliceArgs(int argc, char* const argv[]) {
    std::vector<std::string_view> out;
    if (argc <= 1) return out;
    out.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
        out.emplace_back(argv[i]);
    }
    return out;
}

} // namespace

Action ArgumentParser::parse(int argc, char* const argv[]) {
    if (argc < 2) {
        return ShowHelp{HelpTarget::General};
    }

    const auto args = sliceArgs(argc, argv);
    const std::string_view first = args.front();

    if (first == "help") {
        const HelpTarget target = args.size() >= 2
            ? helpTargetFromToken(args[1])
            : HelpTarget::General;
        return ShowHelp{target};
    }

    if (first == "daemon") {
        return parseDaemonOptions(
            std::vector<std::string_view>(args.begin() + 1, args.end()));
    }

    if (isHelpFlag(first)) {
        if (args.size() > 1) {
            return ParseError{"Error: unexpected argument after " + std::string(first) +
                              ": '" + std::string(args[1]) + "'"};
        }
        return ShowHelp{HelpTarget::General};
    }

    if (const CommandSpec* spec = findByName(first); spec != nullptr) {
        return parseClientCommand(*spec,
            std::vector<std::string_view>(args.begin() + 1, args.end()));
    }

    return ParseError{"Error: unknown command: '" + std::string(first) +
                      "'\nRun '" + std::string(argv[0]) + " help' for usage."};
}

} // namespace cec_control
