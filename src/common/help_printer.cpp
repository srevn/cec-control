#include "help_printer.h"

#include "command_registry.h"
#include "system_paths.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>

namespace cec_control {

namespace {

/**
 * Format one command line as `  NAME ARGSYNTAX     HELP`, with the help
 * column padded to a width derived from the widest name+argSyntax in the
 * registry. Output is pure: no I/O.
 */
std::string renderCommandLine(const CommandSpec& spec, std::size_t descriptionColumn) {
    std::ostringstream oss;
    oss << "  " << spec.name;
    if (!spec.argSyntax.empty()) {
        oss << ' ' << spec.argSyntax;
    }
    const std::size_t printed = oss.str().size();
    const std::size_t pad = printed < descriptionColumn ? descriptionColumn - printed : 1;
    oss << std::string(pad, ' ') << spec.help;
    return oss.str();
}

/**
 * The column at which command descriptions begin. We compute it from the
 * widest entry so adding/removing a command doesn't require re-tuning.
 */
std::size_t computeDescriptionColumn() {
    constexpr std::size_t kLeadingIndent = 2;
    constexpr std::size_t kGap = 4;
    std::size_t maxLeftWidth = 0;
    for (const auto& spec : kCommands) {
        // name + (space + argSyntax) when the syntax is non-empty
        const std::size_t width = spec.name.size() +
            (spec.argSyntax.empty() ? 0 : 1 + spec.argSyntax.size());
        maxLeftWidth = std::max(maxLeftWidth, width);
    }
    return kLeadingIndent + maxLeftWidth + kGap;
}

void printRegistryCommands(std::ostream& out) {
    const std::size_t col = computeDescriptionColumn();
    for (const auto& spec : kCommands) {
        out << renderCommandLine(spec, col) << '\n';
    }
}

} // namespace

void HelpPrinter::printHelp(HelpTarget target, const char* programName) {
    switch (target) {
        case HelpTarget::Client:  printClientHelp(programName);  return;
        case HelpTarget::Daemon:  printDaemonHelp(programName);  return;
        case HelpTarget::General: printGeneralHelp(programName); return;
    }
    printGeneralHelp(programName);
}

void HelpPrinter::printGeneralHelp(const char* programName) {
    std::cout << "CEC Control - HDMI-CEC device management\n"
              << "\n"
              << "USAGE:\n"
              << "  " << programName << " COMMAND [ARGS...] [OPTIONS]    # Client mode\n"
              << "  " << programName << " daemon [OPTIONS]               # Daemon mode\n"
              << "\n"
              << "CLIENT COMMANDS:\n";
    printRegistryCommands(std::cout);
    std::cout << "\n"
              << "DAEMON OPTIONS:\n"
              << "  -v, --verbose                            Enable verbose logging\n"
              << "  -f, --foreground                         Run in foreground (don't daemonize)\n"
              << "  -l, --log FILE                           Set log file path\n"
              << "  -c, --config FILE                        Set configuration file\n"
              << "\n"
              << "DETAILED HELP:\n"
              << "  " << programName << " help client        Show client command reference\n"
              << "  " << programName << " help daemon        Show daemon configuration reference\n"
              << "\n"
              << "EXAMPLES:\n"
              << "  " << programName << " power on 0         Turn on TV\n"
              << "  " << programName << " daemon             Run daemon\n"
              << std::endl;
}

void HelpPrinter::printClientHelp(const char* programName) {
    std::cout << "CEC Client - Control CEC devices\n"
              << "\n"
              << "USAGE:\n"
              << "  " << programName << " COMMAND [ARGS...] [OPTIONS]\n"
              << "\n"
              << "COMMANDS:\n";
    printRegistryCommands(std::cout);
    std::cout << "\n"
              << "OPTIONS:\n"
              << "  --socket-path=PATH                       Set daemon socket path\n"
              << "                                           (default: " << SystemPaths::getSocketPath() << ")\n"
              << "\n"
              << "ENVIRONMENT:\n"
              << "  CEC_CONTROL_SOCKET                       Override socket path for system service\n"
              << "                                           (use /run/cec-control/socket)\n"
              << "\n"
              << "EXAMPLES:\n"
              << "  " << programName << " volume up 5        Increase volume on device 5\n"
              << "  " << programName << " power on 0         Turn on TV (device 0)\n"
              << "  " << programName << " source 0 4         Switch TV to HDMI 3\n"
              << "  " << programName << " suspend            Prepare for system sleep\n"
              << "  CEC_CONTROL_SOCKET=/run/cec-control/socket " << programName << " power on 0\n"
              << "\n"
              << "DEVICE IDs (CEC logical addresses):\n"
              << "  0  - TV                                  5  - Audio System\n"
              << "  1  - Recording Device 1                  4  - Playback Device 1\n"
              << "\n"
              << "SOURCE IDs (input sources):\n"
              << "  0  - General AV input                    3  - HDMI 2\n"
              << "  1  - Audio input                         4  - HDMI 3\n"
              << "  2  - HDMI 1                              5  - HDMI 4\n"
              << std::endl;
}

void HelpPrinter::printDaemonHelp(const char* programName) {
    std::cout << "CEC Daemon - Background service for CEC device management\n"
              << "\n"
              << "USAGE:\n"
              << "  " << programName << " daemon [OPTIONS]\n"
              << "\n"
              << "OPTIONS:\n"
              << "  -v, --verbose                            Enable verbose logging\n"
              << "  -f, --foreground                         Run in foreground (don't daemonize)\n"
              << "  -l, --log FILE                           Set log file path\n"
              << "                                           (default: " << SystemPaths::getLogPath() << ")\n"
              << "  -c, --config FILE                        Set configuration file path\n"
              << "                                           (default: " << SystemPaths::getConfigPath() << ")\n"
              << "  -h, --help                               Show this help message\n"
              << "\n"
              << "EXAMPLES:\n"
              << "  " << programName << " daemon                            Run daemon in background\n"
              << "  " << programName << " daemon --verbose --foreground     Run with verbose logging in foreground\n"
              << "  " << programName << " daemon -c /path/to/config.conf    Run with custom configuration\n"
              << std::endl;
}

} // namespace cec_control
