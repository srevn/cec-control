#include "cec_daemon.h"
#include "../common/logger.h"
#include "../common/config_manager.h"
#include "../common/xdg_paths.h"

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>

// Use the cec_control namespace for convenience with XDGPaths
using cec_control::RuntimeEnvironment;

/**
 * Daemonize the process based on runtime environment.
 * 
 * @param verboseMode If true, keep stdout/stderr open for logging
 * @return True if we're running in the daemon context, false if we should exit (parent)
 */
bool daemonize(bool verboseMode) {
    // Detect the runtime environment
    RuntimeEnvironment env = cec_control::XDGPaths::getEnvironment();
    
    // Log based on where we're running
    switch (env) {
        case RuntimeEnvironment::SYSTEM_SERVICE:
            LOG_INFO("Running as system service, not daemonizing");
            break;
        case RuntimeEnvironment::USER_SERVICE:
            LOG_INFO("Running as user service, not daemonizing");
            break;
        case RuntimeEnvironment::NORMAL_USER:
            LOG_INFO("Running as normal user application, will daemonize if requested");
            break;
    }
    
    // Under systemd or other service managers, do not fork
    if (env != RuntimeEnvironment::NORMAL_USER) {
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
    
    // Only daemonize in normal user mode when requested
    if (!verboseMode) {
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
    } else {
        // In foreground mode, just close stdin
        close(STDIN_FILENO);
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        if (null > 0) {
            close(null);
        }
        
        // Keep stdout/stderr open for terminal output
        LOG_INFO("Running in foreground mode");
    }
    
    return true; // We're the daemon process
}

int main(int argc, char* argv[]) {
    bool verboseMode = false;
    bool runAsDaemon = true;
    std::string logFile = cec_control::XDGPaths::getLogPath();
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
                      << "  -l, --log FILE          Log to FILE (default: " << cec_control::XDGPaths::getLogPath() << ")\n"
                      << "  -c, --config FILE       Set configuration file path\n"
                      << "                          (default: " << cec_control::XDGPaths::getConfigPath() << ")\n"
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
    cec_control::ConfigManager* configManager;
    
    if (configFile.empty()) {
        configManager = new cec_control::ConfigManager();
    } else {
        configManager = new cec_control::ConfigManager(configFile);
    }
    
    configManager->load();
    
    // Only daemonize if runAsDaemon is true
    if (runAsDaemon) {
        // daemonize returns false for the parent process that should exit
        if (!daemonize(verboseMode)) {
            LOG_INFO("Parent process exiting, daemon started");
            return EXIT_SUCCESS;
        }
        LOG_INFO("Daemon process started");
    } else {
        LOG_INFO("Running in foreground mode");
        // When not running as daemon, ensure stdin is still redirected
        close(STDIN_FILENO);
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDIN_FILENO);
        if (null > 0) {
            close(null);
        }
    }
    
    // Log runtime environment and PID
    LOG_INFO("Running with PID: ", getpid(), " in ", 
             cec_control::XDGPaths::getEnvironment() == RuntimeEnvironment::SYSTEM_SERVICE ? "system service" : 
             cec_control::XDGPaths::getEnvironment() == RuntimeEnvironment::USER_SERVICE ? "user service" : 
             "normal user mode");
    
    // Create daemon with options
    cec_control::CECDaemon::Options options;
    options.scanDevicesAtStartup = configManager->getBool("Daemon", "ScanDevicesAtStartup", false);
    options.queueCommandsDuringSuspend = configManager->getBool("Daemon", "QueueCommandsDuringSuspend", true);
    
    cec_control::CECDaemon daemon(options);
    if (!daemon.start()) {
        LOG_FATAL("Failed to start CEC daemon");
        return EXIT_FAILURE;
    }
    
    // Run the main loop
    daemon.run();
    
    delete configManager;
    
    return EXIT_SUCCESS;
}
