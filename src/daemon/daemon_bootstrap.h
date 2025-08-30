#pragma once

#include "cec_daemon.h"
#include "../common/argument_parser.h"
#include "../common/config_manager.h"

namespace cec_control {

/**
 * Bootstrap class responsible for daemon process management and initialization
 * Extracts all process-level functionality from main.cpp
 */
class DaemonBootstrap {
public:
    /**
     * Initialize and run the CEC daemon with the given configuration
     * @param parseResult Parsed command line arguments and configuration
     * @return Exit code (EXIT_SUCCESS or EXIT_FAILURE)
     */
    static int runDaemon(const ArgumentParser::ParseResult& parseResult);
    
private:
    /**
     * Setup the process (daemonization, service mode, etc.)
     * @param runAsDaemon Whether to daemonize the process
     * @return true if setup succeeded
     */
    static bool setupProcess(bool runAsDaemon);
    
    /**
     * Check if running under systemd
     * @return true if running under systemd
     */
    static bool isRunningUnderSystemd();
    
    /**
     * Daemonize the process by forking into background
     * @return true if we're in daemon context, false if parent should exit
     */
    static bool daemonize();
    
    /**
     * Set up process for systemd service
     * @return true if setup succeeded
     */
    static bool setupService();
    
    /**
     * Initialize logging with the given configuration
     * @param parseResult Configuration from argument parsing
     */
    static void setupLogging(const ArgumentParser::ParseResult& parseResult);
    
    /**
     * Load and setup configuration management
     * @param parseResult Configuration from argument parsing
     * @return Reference to configured ConfigManager
     */
    static ConfigManager& setupConfiguration(const ArgumentParser::ParseResult& parseResult);
    
    /**
     * Create daemon options from loaded configuration
     * @param configManager Loaded configuration manager
     * @return CECDaemon options structure
     */
    static CECDaemon::Options createDaemonOptions(const ConfigManager& configManager);
};

} // namespace cec_control