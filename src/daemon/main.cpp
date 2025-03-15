#include "cec_daemon.h"
#include "../common/logger.h"
#include "../common/config_manager.h"
#include "../common/xdg_paths.h"

#include <iostream>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

void daemonize() {
    // Fork and let parent exit
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork daemon process" << std::endl;
        exit(EXIT_FAILURE);
    }
    
    // Parent exits
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    // Create new session
    if (setsid() < 0) {
        std::cerr << "Failed to create new session" << std::endl;
        exit(EXIT_FAILURE);
    }
    
    // Fork again to prevent reacquiring terminal
    pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork daemon process (2nd fork)" << std::endl;
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }
    
    // Set new file permissions
    umask(0);
    
    // Change working directory
    chdir("/");
    
    // Close standard file descriptors
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
}

int main(int argc, char* argv[]) {
    bool runAsDaemon = true;
    bool scanDevicesAtStartup = false;
    std::string logFile = cec_control::XDGPaths::getDefaultLogPath();
    std::string configFile;  // Empty means use default
    
    // Process command line options
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--verbose" || arg == "-v") {
            runAsDaemon = false;
        }
        else if (arg == "--scan-devices" || arg == "-s") {
            scanDevicesAtStartup = true;
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
                      << "  -v, --verbose           Run in foreground (don't daemonize)\n"
                      << "  -l, --log FILE          Log to FILE (default: " << cec_control::XDGPaths::getDefaultLogPath() << ")\n"
                      << "  -s, --scan-devices      Scan for CEC devices at startup\n"
                      << "  -c, --config FILE       Set configuration file path\n"
                      << "                          (default: " << cec_control::XDGPaths::getDefaultConfigPath() << ")\n"
                      << "  -h, --help              Show this help message\n";
            return EXIT_SUCCESS;
        }
    }
    
    // Initialize the configuration manager
    cec_control::ConfigManager* configManager;
    
    if (configFile.empty()) {
        configManager = new cec_control::ConfigManager();
    } else {
        configManager = new cec_control::ConfigManager(configFile);
    }
    
    configManager->load();
    
    // Configure logging
    cec_control::Logger::getInstance().setLogFile(logFile);
    cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::INFO);
    
    // Daemonize if requested
    if (runAsDaemon) {
        daemonize();
    }
    
    // Create daemon with options
    cec_control::CECDaemon::Options options;
    options.scanDevicesAtStartup = scanDevicesAtStartup;
    
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
