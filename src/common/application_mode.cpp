#include "application_mode.h"
#include <unordered_set>

namespace cec_control {

ApplicationMode ModeDetector::detectMode(int argc, char* argv[]) {
    // Handle no arguments case
    if (argc < 2) {
        return ApplicationMode::HELP_GENERAL;
    }

    // First check for help commands (highest priority)
    std::string firstArg = argv[1];
    if (firstArg == "help") {
        if (argc >= 3) {
            std::string helpTarget = argv[2];
            if (helpTarget == "client") {
                return ApplicationMode::HELP_CLIENT;
            } else if (helpTarget == "daemon") {
                return ApplicationMode::HELP_DAEMON;
            }
        }
        return ApplicationMode::HELP_GENERAL;
    }

    // Check for --help flag (context-sensitive)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            // If we also have --daemon or -d flag, show daemon help
            for (int j = 1; j < argc; ++j) {
                std::string daemonArg = argv[j];
                if (daemonArg == "--daemon" || daemonArg == "-d") {
                    return ApplicationMode::HELP_DAEMON;
                }
            }
            // If first argument is a client command, show client help
            if (isClientCommand(firstArg)) {
                return ApplicationMode::HELP_CLIENT;
            }
            return ApplicationMode::HELP_GENERAL;
        }
    }

    // Check for explicit daemon flag
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--daemon" || arg == "-d") {
            return ApplicationMode::DAEMON;
        }
    }

    // Check if first argument is a client command
    if (isClientCommand(firstArg)) {
        return ApplicationMode::CLIENT;
    }

    // Check if first argument looks like a daemon option
    if (isDaemonOption(firstArg)) {
        return ApplicationMode::DAEMON;
    }

    // Default to daemon mode
    return ApplicationMode::DAEMON;
}

bool ModeDetector::isClientCommand(const std::string& arg) {
    static const std::unordered_set<std::string> clientCommands = {
        "volume",
        "power",
        "source",
        "auto-standby",
        "restart",
        "suspend",
        "resume"
    };

    return clientCommands.find(arg) != clientCommands.end();
}

bool ModeDetector::isDaemonOption(const std::string& arg) {
    static const std::unordered_set<std::string> daemonOptions = {
        "--verbose", "-v",
        "--foreground", "-f",
        "--log", "-l",
        "--config", "-c",
        "--help", "-h",
        "--daemon", "-d"
    };

    return daemonOptions.find(arg) != daemonOptions.end();
}

} // namespace cec_control
