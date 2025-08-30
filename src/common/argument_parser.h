#pragma once

#include "application_mode.h"
#include "messages.h"
#include "system_paths.h"
#include <string>
#include <optional>

namespace cec_control {

class ArgumentParser {
public:
    /**
     * Result of argument parsing containing all necessary information
     */
    struct ParseResult {
        ApplicationMode mode;
        bool showHelp;
        std::string errorMessage;
        bool hasError;
        
        // Client-specific fields
        std::optional<Message> clientCommand;
        std::string socketPath;
        
        // Daemon-specific fields  
        bool verboseMode;
        bool runAsDaemon;
        std::string logFile;
        std::string configFile;
        
        ParseResult() 
            : mode(ApplicationMode::HELP_GENERAL),
              showHelp(false),
              hasError(false),
              socketPath(SystemPaths::getSocketPath()),
              verboseMode(false),
              runAsDaemon(true),
              logFile(SystemPaths::getLogPath()),
              configFile("") {}
    };
    
    /**
     * Parse command line arguments and return complete result
     * @param argc Argument count
     * @param argv Argument vector  
     * @return ParseResult with all parsed information
     */
    static ParseResult parse(int argc, char* argv[]);
    
private:
    /**
     * Parse arguments when in client mode
     * @param argc Argument count
     * @param argv Argument vector
     * @return ParseResult for client mode
     */
    static ParseResult parseClientArgs(int argc, char* argv[]);
    
    /**
     * Parse arguments when in daemon mode
     * @param argc Argument count
     * @param argv Argument vector
     * @return ParseResult for daemon mode
     */
    static ParseResult parseDaemonArgs(int argc, char* argv[]);
    
    /**
     * Parse arguments when help is requested
     * @param mode The specific help mode requested
     * @return ParseResult for help display
     */
    static ParseResult parseHelpArgs(ApplicationMode mode);
    
    /**
     * Validate that argument count is sufficient for a command
     * @param argc Current argument count
     * @param expectedCount Expected minimum count
     * @param commandName Name of the command for error messages
     * @return true if valid, false otherwise
     */
    static bool validateArgCount(int argc, int expectedCount, const std::string& commandName, std::string& errorMsg);
    
    /**
     * Extract socket path override from arguments
     * @param argc Argument count
     * @param argv Argument vector
     * @param socketPath Reference to socketPath to update
     */
    static void extractSocketPath(int argc, char* argv[], std::string& socketPath);
};

} // namespace cec_control