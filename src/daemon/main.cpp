#include "cec_daemon.h"
#include "../common/logger.h"
#include "../common/config_manager.h"
#include "../common/system_paths.h"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

// Use the cec_control namespace for convenience with SystemPaths

/**
 * Set up process for systemd service
 * 
 * @param verboseMode If true, keep stdout/stderr open for logging
 * @return Always returns true
 */
bool setupService(bool verboseMode) {
    LOG_INFO("Running as system service");
    
    // Redirect stdin to /dev/null (we don't need it)
    close(STDIN_FILENO);
    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    if (null > 0) {
        close(null);
    }
    
    // Keep stdout/stderr open for service manager journal
    return true;
}

/**
 * Daemonize the process by forking into the background
 * 
 * @param verboseMode If true, keep stdout/stderr open for logging
 * @return True if we're running in the daemon context, false if we should exit (parent)
 */
bool daemonize(bool verboseMode) {
    // Set reasonable file permissions
    umask(022);
    
    // Fork and let parent exit
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork daemon process" << std::endl;
        exit(EXIT_FAILURE);
    }
    
    // Parent exits
    if (pid > 0) {
        // We're the parent process - exit with successful status
        return false;
    }
    
    // Create new session
    if (setsid() < 0) {
        std::cerr << "Failed to create new session" << std::endl;
        exit(EXIT_FAILURE);
    }
    
    // Second fork to detach from terminal
    pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork daemon process (2nd fork)" << std::endl;
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        // Parent exits
        exit(EXIT_SUCCESS);
    }
    
    // Change working directory
    chdir("/");
    
    // Normal daemon mode - close all descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    // Redirect standard file descriptors to /dev/null
    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    if (null > 2) {
        close(null);
    }
    
    return true; // We're the daemon process
}

int main(int argc, char* argv[]) {
    bool verboseMode = false;
    bool runAsDaemon = true;
    std::string logFile = cec_control::SystemPaths::getLogPath();
    std::string configFile;

    // Process command line options
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            verboseMode = true;
        }
        else if (arg == "--foreground" || arg == "-f") {
            runAsDaemon = false;
        }
        else if ((arg == "--log" || arg == "-l") && i + 1 < argc) {
            logFile = argv[++i];
        }
        else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            configFile = argv[++i];
        }
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                      << "Options:\n"
                      << "  -v, --verbose           Enable verbose logging (to console and log file)\n"
                      << "  -f, --foreground        Run in foreground (don't daemonize)\n"
                      << "  -l, --log FILE          Log to FILE (default: " << cec_control::SystemPaths::getLogPath() << ")\n"
                      << "  -c, --config FILE       Set configuration file path\n"
                      << "                          (default: " << cec_control::SystemPaths::getConfigPath() << ")\n"
                      << "  -h, --help              Show this help message\n";
            return EXIT_SUCCESS;
        }
    }
    // Configure logging
    cec_control::Logger::getInstance().setLogFile(logFile);
    
    // Set log level based on verbose mode
    if (verboseMode) {
        cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::DEBUG);
    } else {
        cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::INFO);
    }
    
    // Initialize the configuration manager
    cec_control::ConfigManager& configManager = cec_control::ConfigManager::getInstance();

    // Set custom config path if provided
    if (!configFile.empty()) {
        configManager = cec_control::ConfigManager(configFile);
    }

    // Load the configuration
    if (!configManager.load()) {
        LOG_WARNING("Failed to load configuration file, using defaults");
    }
    
    // Check if running under systemd
    const char* notifySocket = getenv("NOTIFY_SOCKET");
    bool runningUnderSystemd = (notifySocket && *notifySocket);
    
    // Only daemonize if requested and not running under systemd
    if (runAsDaemon && !runningUnderSystemd) {
        LOG_INFO("Running as normal executable, will daemonize");
        // daemonize returns false for the parent process that should exit
        if (!daemonize(verboseMode)) {
            LOG_INFO("Parent process exiting, daemon started");
            return EXIT_SUCCESS;
        }
        LOG_INFO("Daemon process started with PID: ", getpid());
    } else {
        if (runningUnderSystemd) {
            LOG_INFO("Running under systemd, not daemonizing");
        } else {
            LOG_INFO("Running in foreground mode");
        }
        
        // When running as service or in foreground, just redirect stdin
        close(STDIN_FILENO);
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        if (null > 0) {
            close(null);
        }
    }
    
    // Log runtime environment and PID
    LOG_INFO("Running with PID: ", getpid(), " in system service mode");
    
    // Create daemon with options
    cec_control::CECDaemon::Options options;
    options.scanDevicesAtStartup = configManager.getBool("Daemon", "ScanDevicesAtStartup", false);
    options.queueCommandsDuringSuspend = configManager.getBool("Daemon", "QueueCommandsDuringSuspend", true);
    options.enablePowerMonitor = configManager.getBool("Daemon", "EnablePowerMonitor", true);
    
    cec_control::CECDaemon daemon(options);
    if (!daemon.start()) {
        LOG_FATAL("Failed to start CEC daemon");
        return EXIT_FAILURE;
    }
    
    // Run the main loop
    daemon.run();
    
    return EXIT_SUCCESS;
}
