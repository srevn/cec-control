#include "argument_parser.h"
#include "../client/command_mapper.h"
#include <iostream>

namespace cec_control {

ArgumentParser::ParseResult ArgumentParser::parse(int argc, char* argv[]) {
    // Detect the application mode first
    ApplicationMode mode = ModeDetector::detectMode(argc, argv);
    
    // Handle help modes
    if (mode == ApplicationMode::HELP_GENERAL || 
        mode == ApplicationMode::HELP_CLIENT || 
        mode == ApplicationMode::HELP_DAEMON) {
        return parseHelpArgs(mode);
    }
    
    // Parse based on detected mode
    if (mode == ApplicationMode::CLIENT) {
        return parseClientArgs(argc, argv);
    } else {
        return parseDaemonArgs(argc, argv);
    }
}

ArgumentParser::ParseResult ArgumentParser::parseClientArgs(int argc, char* argv[]) {
    ParseResult result;
    result.mode = ApplicationMode::CLIENT;
    
    // Extract socket path override if present
    extractSocketPath(argc, argv, result.socketPath);
    
    // Parse the command (first argument)
    std::string command = argv[1];
    
    // Validate and map commands using the same logic as CECClient
    if (command == "volume") {
        if (!validateArgCount(argc, 4, command, result.errorMessage)) {
            result.hasError = true;
            return result;
        }
        result.clientCommand = CommandMapper::mapVolumeCommand(argv[2], argv[3]);
    }
    else if (command == "power") {
        if (!validateArgCount(argc, 4, command, result.errorMessage)) {
            result.hasError = true;
            return result;
        }
        result.clientCommand = CommandMapper::mapPowerCommand(argv[2], argv[3]);
    }
    else if (command == "source") {
        if (!validateArgCount(argc, 4, command, result.errorMessage)) {
            result.hasError = true;
            return result;
        }
        result.clientCommand = CommandMapper::mapSourceCommand(argv[2], argv[3]);
    }
    else if (command == "auto-standby") {
        if (!validateArgCount(argc, 3, command, result.errorMessage)) {
            result.hasError = true;
            return result;
        }
        result.clientCommand = CommandMapper::mapAutoStandbyCommand(argv[2]);
    }
    else if (command == "restart") {
        result.clientCommand = CommandMapper::mapRestartCommand();
    }
    else if (command == "suspend") {
        result.clientCommand = CommandMapper::mapSuspendCommand();
    }
    else if (command == "resume") {
        result.clientCommand = CommandMapper::mapResumeCommand();
    }
    else {
        result.hasError = true;
        result.errorMessage = "Error: Unknown command: " + command;
        return result;
    }
    
    // Check if command mapping failed
    if (!result.clientCommand.has_value()) {
        result.hasError = true;
        result.errorMessage = "Error: Failed to parse command arguments";
        return result;
    }
    
    return result;
}

ArgumentParser::ParseResult ArgumentParser::parseDaemonArgs(int argc, char* argv[]) {
    ParseResult result;
    result.mode = ApplicationMode::DAEMON;
    
    // Process daemon arguments using the same logic as daemon/main.cpp
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--verbose" || arg == "-v") {
            result.verboseMode = true;
        }
        else if (arg == "--foreground" || arg == "-f") {
            result.runAsDaemon = false;
        }
        else if ((arg == "--log" || arg == "-l") && i + 1 < argc) {
            result.logFile = argv[++i];
            
            // Validate log file path
            if (result.logFile.empty()) {
                result.hasError = true;
                result.errorMessage = "Error: Empty log file path provided";
                return result;
            }
        }
        else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            result.configFile = argv[++i];
            
            // Validate config file path
            if (result.configFile.empty()) {
                result.hasError = true;
                result.errorMessage = "Error: Empty config file path provided";
                return result;
            } else if (!SystemPaths::pathExists(result.configFile)) {
                // This is just a warning, not an error - preserve the same behavior
                std::cerr << "Warning: Config file does not exist: " << result.configFile << std::endl;
            }
        }
        else if (arg == "--help" || arg == "-h") {
            result.showHelp = true;
        }
        else if (arg == "--daemon" || arg == "-d") {
            // This is already handled by mode detection, just ignore it here
            continue;
        }
        else {
            result.hasError = true;
            result.errorMessage = "Error: Unknown option: " + arg;
            return result;
        }
    }
    
    return result;
}

ArgumentParser::ParseResult ArgumentParser::parseHelpArgs(ApplicationMode mode) {
    ParseResult result;
    result.mode = mode;
    result.showHelp = true;
    return result;
}

bool ArgumentParser::validateArgCount(int argc, int expectedCount, const std::string& commandName, std::string& errorMsg) {
    if (argc < expectedCount) {
        errorMsg = "Error: " + commandName + " command requires " + std::to_string(expectedCount - 2) + " argument(s)";
        return false;
    }
    return true;
}

void ArgumentParser::extractSocketPath(int argc, char* argv[], std::string& socketPath) {
    // Extract socket path override using the same logic as CECClient
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.substr(0, 13) == "--socket-path=") {
            socketPath = arg.substr(13);
            break;
        }
    }
}

} // namespace cec_control