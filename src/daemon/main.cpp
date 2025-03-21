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
 * @return Always returns true
 */
bool setupService() {
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

/**
 * Daemonize the process by forking into the background
 * 
 * @return True if we're running in the daemon context, false if we should exit (parent)
 */
bool daemonize() {
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

/**
 * Structure to hold command line arguments
 */
struct ProgramOptions {
    bool verboseMode = false;
    bool runAsDaemon = true;
    bool showHelp = false;
    std::string logFile;
    std::string configFile;
};

/**
 * Parse command line arguments
 * 
 * @param argc Argument count
 * @param argv Argument values
 * @return Parsed program options
 */
ProgramOptions parseCommandLine(int argc, char* argv[]) {
    ProgramOptions options;
    
    // Set default paths
    options.logFile = cec_control::SystemPaths::getLogPath();
    
    // Process command line options
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--verbose" || arg == "-v") {
            options.verboseMode = true;
        }
        else if (arg == "--foreground" || arg == "-f") {
            options.runAsDaemon = false;
        }
        else if ((arg == "--log" || arg == "-l") && i + 1 < argc) {
            options.logFile = argv[++i];
            
            // Validate log file path
            if (options.logFile.empty()) {
                std::cerr << "Error: Empty log file path provided" << std::endl;
                options.showHelp = true;
            }
        }
        else if ((arg == "--config" || arg == "-c") && i + 1 < argc) {
            options.configFile = argv[++i];
            
            // Validate config file path
            if (options.configFile.empty()) {
                std::cerr << "Error: Empty config file path provided" << std::endl;
                options.showHelp = true;
            } else if (!cec_control::SystemPaths::pathExists(options.configFile)) {
                std::cerr << "Warning: Config file does not exist: " << options.configFile << std::endl;
                // Don't set showHelp, as this is just a warning
            }
        }
        else if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        }
        else {
            std::cerr << "Error: Unknown option: " << arg << std::endl;
            options.showHelp = true;
        }
    }
    
    return options;
}

/**
 * Print usage information
 * 
 * @param programName The name of the program
 */
void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " [OPTIONS]\n"
              << "Options:\n"
              << "  -v, --verbose           Enable verbose logging (to console and log file)\n"
              << "  -f, --foreground        Run in foreground (don't daemonize)\n"
              << "  -l, --log FILE          Log to FILE (default: " << cec_control::SystemPaths::getLogPath() << ")\n"
              << "  -c, --config FILE       Set configuration file path\n"
              << "                          (default: " << cec_control::SystemPaths::getConfigPath() << ")\n"
              << "  -h, --help              Show this help message\n";
}

/**
 * Initialize and configure the logger
 * 
 * @param options ProgramOptions from command line
 */
void setupLogging(const ProgramOptions& options) {
    // Configure log file
    cec_control::Logger::getInstance().setLogFile(options.logFile);
    
    // Set log level based on verbose mode
    if (options.verboseMode) {
        cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::DEBUG);
    } else {
        cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::INFO);
    }
    
    LOG_INFO("Logging initialized with file: ", options.logFile);
}

/**
 * Load configuration and set up the config manager
 * 
 * @param options ProgramOptions from command line
 * @return ConfigManager with loaded configuration
 */
cec_control::ConfigManager& setupConfiguration(const ProgramOptions& options) {
    cec_control::ConfigManager& configManager = cec_control::ConfigManager::getInstance();
    
    // Set custom config path if provided
    if (!options.configFile.empty()) {
        configManager = cec_control::ConfigManager(options.configFile);
    }
    
    // Load the configuration
    if (!configManager.load()) {
        LOG_WARNING("Failed to load configuration file, using defaults");
    }
    
    return configManager;
}

/**
 * Create CEC daemon options from configuration
 * 
 * @param configManager Loaded configuration manager
 * @return CECDaemon::Options daemon options
 */
cec_control::CECDaemon::Options createDaemonOptions(const cec_control::ConfigManager& configManager) {
    cec_control::CECDaemon::Options options;

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

/**
 * Check if running under systemd
 * 
 * @return True if running under systemd
 */
bool isRunningUnderSystemd() {
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

int main(int argc, char* argv[]) {
    // Parse command line arguments
    ProgramOptions options = parseCommandLine(argc, argv);
    
    // Check if help is requested
    if (options.showHelp) {
        printUsage(argv[0]);
        return EXIT_SUCCESS;
    }
    
    // Set up logging first so we can log any issues
    setupLogging(options);
    
    // Load configuration
    cec_control::ConfigManager& configManager = setupConfiguration(options);
    
    // Check if running under systemd
    bool runningUnderSystemd = isRunningUnderSystemd();
    
    // Determine if we should daemonize
    if (options.runAsDaemon && !runningUnderSystemd) {
        LOG_INFO("Running as normal executable, will daemonize");
        
        // daemonize returns false for the parent process that should exit
        if (!daemonize()) {
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
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null > STDIN_FILENO) {
                close(null);
            }
        } else {
            LOG_WARNING("Failed to open /dev/null: ", strerror(errno));
        }
    }
    
    // Log runtime environment and PID
    LOG_INFO("Running with PID: ", getpid(), " in system service mode");
    
    // Create daemon with options from config
    cec_control::CECDaemon::Options daemonOptions = createDaemonOptions(configManager);
    
    try {
        // Create and start daemon
        cec_control::CECDaemon daemon(daemonOptions);
        
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
