#include "daemon_bootstrap.h"
#include "../common/logger.h"
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

namespace cec_control {

int DaemonBootstrap::runDaemon(const ArgumentParser::ParseResult& parseResult) {
    // Set up logging first so we can log any issues
    setupLogging(parseResult);
    
    // Load configuration
    ConfigManager& configManager = setupConfiguration(parseResult);
    
    // Setup the process (daemonization, service mode, etc.)
    if (!setupProcess(parseResult.runAsDaemon)) {
        LOG_FATAL("Failed to setup daemon process");
        return EXIT_FAILURE;
    }
    
    // Log runtime environment and PID
    LOG_INFO("Running with PID: ", getpid(), " in system service mode");
    
    // Create daemon with options from config
    CECDaemon::Options daemonOptions = createDaemonOptions(configManager);
    
    try {
        // Create and start daemon
        CECDaemon daemon(daemonOptions);
        
        if (!daemon.start()) {
            LOG_FATAL("Failed to start CEC daemon");
            return EXIT_FAILURE;
        }
        
        // Run the main loop
        LOG_INFO("CEC daemon initialized successfully, starting main loop");
        daemon.run();
        
        // Should only get here if daemon.run() returns
        LOG_INFO("CEC daemon exited normally");
        return EXIT_SUCCESS;
    } 
    catch (const std::exception& e) {
        LOG_FATAL("Exception in CEC daemon: ", e.what());
        return EXIT_FAILURE;
    }
    catch (...) {
        LOG_FATAL("Unknown exception in CEC daemon");
        return EXIT_FAILURE;
    }
}

bool DaemonBootstrap::setupProcess(bool runAsDaemon) {
    // Check if running under systemd
    bool runningUnderSystemd = isRunningUnderSystemd();
    
    // Determine if we should daemonize
    if (runAsDaemon && !runningUnderSystemd) {
        LOG_INFO("Running as normal executable, will daemonize");
        
        // daemonize returns false for the parent process that should exit
        if (!daemonize()) {
            LOG_INFO("Parent process exiting, daemon started");
            exit(EXIT_SUCCESS);
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
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null > STDIN_FILENO) {
                close(null);
            }
        } else {
            LOG_WARNING("Failed to open /dev/null: ", strerror(errno));
        }
    }
    
    return true;
}

bool DaemonBootstrap::isRunningUnderSystemd() {
    // Primary method: Check for NOTIFY_SOCKET environment variable
    const char* notifySocket = getenv("NOTIFY_SOCKET");
    if (notifySocket && *notifySocket) {
        return true;
    }
    
    // Secondary method: Check for INVOCATION_ID (set for systemd services)
    const char* invocationId = getenv("INVOCATION_ID");
    if (invocationId && *invocationId) {
        return true;
    }
    
    // Tertiary method: Check for SYSTEMD_EXEC_PID (should be set to our PID)
    const char* execPid = getenv("SYSTEMD_EXEC_PID");
    if (execPid && *execPid) {
        return true;
    }
    
    return false;
}

bool DaemonBootstrap::daemonize() {
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
    return true; // We're the daemon process
}

bool DaemonBootstrap::setupService() {
    LOG_INFO("Running as system service");

    // Redirect stdin to /dev/null (we don't need it)
    if (close(STDIN_FILENO) != 0) {
        LOG_ERROR("Failed to close STDIN: ", strerror(errno));
        return false;
    }
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0) {
        LOG_ERROR("Failed to open /dev/null: ", strerror(errno));
        return false;
    }
    if (dup2(null_fd, STDIN_FILENO) < 0) {
        LOG_ERROR("Failed to redirect /dev/null to STDIN: ", strerror(errno));
        close(null_fd); // Close null_fd in case dup2 failed
        return false;
    }

    // Keep stdout/stderr open for service manager journal
    return true;
}

void DaemonBootstrap::setupLogging(const ArgumentParser::ParseResult& parseResult) {
    // Configure log file
    Logger::getInstance().setLogFile(parseResult.logFile);
    
    // Set log level based on verbose mode
    if (parseResult.verboseMode) {
        Logger::getInstance().setLogLevel(LogLevel::DEBUG);
    } else {
        Logger::getInstance().setLogLevel(LogLevel::INFO);
    }
    
    LOG_INFO("Logging initialized with file: ", parseResult.logFile);
}

ConfigManager& DaemonBootstrap::setupConfiguration(const ArgumentParser::ParseResult& parseResult) {
    // First call to getInstance will initialize it with the provided config path.
    ConfigManager& configManager = ConfigManager::getInstance(parseResult.configFile);
    
    // Load the configuration
    if (!configManager.load()) {
        LOG_WARNING("Failed to load configuration file, using defaults");
    }
    
    return configManager;
}

CECDaemon::Options DaemonBootstrap::createDaemonOptions(const ConfigManager& configManager) {
    CECDaemon::Options options;

    // Set daemon options from configuration
    options.scanDevicesAtStartup = configManager.getBool("Daemon", "ScanDevicesAtStartup", false);
    options.queueCommandsDuringSuspend = configManager.getBool("Daemon", "QueueCommandsDuringSuspend", true);
    options.enablePowerMonitor = configManager.getBool("Daemon", "EnablePowerMonitor", true);

    // Log daemon options - use more concise logging with direct boolean values
    LOG_INFO("Configuration: ScanDevicesAtStartup = ", (options.scanDevicesAtStartup ? "true" : "false"));
    LOG_INFO("Configuration: QueueCommandsDuringSuspend = ", (options.queueCommandsDuringSuspend ? "true" : "false"));
    LOG_INFO("Configuration: EnablePowerMonitor = ", (options.enablePowerMonitor ? "true" : "false"));

    return options;
}

} // namespace cec_control