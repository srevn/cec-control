#include "xdg_paths.h"
#include "logger.h"

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace cec_control {

// Define static constants
const std::string XDGPaths::APP_NAME = "cec-control";
const std::string XDGPaths::CONFIG_FILENAME = "config.conf";
const std::string XDGPaths::LOG_FILENAME = "daemon.log";
const std::string XDGPaths::SOCKET_FILENAME = "socket";

// Define these inside the detection and path functions
// so we don't need a global initialization dependency on APP_NAME

// Environment detection function
RuntimeEnvironment XDGPaths::detectEnvironment() {
    // Method 1: Check NOTIFY_SOCKET environment variable (standard systemd detection)
    const char* notifySocket = getenv("NOTIFY_SOCKET");
    if (notifySocket && *notifySocket) {
        // We're running under systemd
        
        // Check if we're running as a user service
        const char* xdgRuntimeDir = getenv("XDG_RUNTIME_DIR");
        if (xdgRuntimeDir && strstr(xdgRuntimeDir, "/run/user/") != nullptr) {
            return RuntimeEnvironment::USER_SERVICE;
        }
        
        // Running as system service
        return RuntimeEnvironment::SYSTEM_SERVICE;
    }
    
    // Method 2: Check for our explicit environment variable
    const char* explicitEnv = getenv("CEC_CONTROL_ENVIRONMENT");
    if (explicitEnv) {
        if (strcmp(explicitEnv, "system_service") == 0) {
            LOG_INFO("Detected system service from explicit environment variable");
            return RuntimeEnvironment::SYSTEM_SERVICE;
        }
        if (strcmp(explicitEnv, "user_service") == 0) {
            LOG_INFO("Detected user service from explicit environment variable");
            return RuntimeEnvironment::USER_SERVICE;
        }
    }
    
    // Method 3: Check systemd-specific environment variables
    if (getenv("INVOCATION_ID") || getenv("RUNTIME_DIRECTORY") || 
        getenv("STATE_DIRECTORY") || getenv("SYSTEMD_EXEC_PID")) {
        // These environment variables are set by systemd when running as a service
        LOG_INFO("Detected systemd environment from systemd-specific variables");
        return RuntimeEnvironment::SYSTEM_SERVICE;
    }
    
    // Method 4: Check parent process name
    // Try to read our parent process name from /proc
    FILE* cmdline = fopen(("/proc/" + std::to_string(getppid()) + "/comm").c_str(), "r");
    if (cmdline) {
        char name[256] = {0};
        if (fgets(name, sizeof(name), cmdline)) {
            LOG_DEBUG("Parent process: ", name);
            // Remove newline if present
            char* nl = strchr(name, '\n');
            if (nl) *nl = '\0';
            
            // Check if systemd is our parent
            if (strcmp(name, "systemd") == 0) {
                LOG_INFO("Detected systemd environment from parent process");
                fclose(cmdline);
                return RuntimeEnvironment::SYSTEM_SERVICE;
            }
        }
        fclose(cmdline);
    }
    
    // Method 5: Check if running as root without HOME environment
    if (getuid() == 0 && !getenv("HOME")) {
        LOG_INFO("Running as root without HOME environment, assuming system service");
        return RuntimeEnvironment::SYSTEM_SERVICE;
    }
    
    // Normal user-level application if none of the above checks pass
    LOG_DEBUG("No systemd environment detected, running as normal user application");
    return RuntimeEnvironment::NORMAL_USER;
}

RuntimeEnvironment XDGPaths::getEnvironment() {
    return detectEnvironment();
}

std::string XDGPaths::getHomeDir() {
    const char* homeDir = getenv("HOME");
    
    if (!homeDir) {
        struct passwd* pwd = getpwuid(getuid());
        if (pwd) {
            homeDir = pwd->pw_dir;
        }
    }
    
    return homeDir ? std::string(homeDir) : "";
}

std::string XDGPaths::getSystemRuntimeDir() {
    // Check if environment has a specific runtime dir from systemd
    const char* runtimeDir = getenv("RUNTIME_DIRECTORY");
    if (runtimeDir && *runtimeDir) {
        // The systemd runtime directory variable already contains the full path under /run
        // Just return the value without prepending /run/ again
        if (strncmp(runtimeDir, "cec-control", 11) == 0) {
            // Simple directory name, add /run/
            return std::string("/run/") + runtimeDir;
        } else if (strncmp(runtimeDir, "/", 1) == 0) {
            // Absolute path, use as is
            return runtimeDir;
        } else {
            // Relative path but not just our app name
            LOG_INFO("Using runtime directory from systemd: ", runtimeDir);
            return std::string("/run/") + runtimeDir;
        }
    }
    
    // Default system runtime directory
    return std::string("/run/") + APP_NAME;
}

std::string XDGPaths::getUserRuntimeDir() {
    std::string runtimeDir;
    const char* xdgRuntimeDir = getenv("XDG_RUNTIME_DIR");
    
    if (xdgRuntimeDir && *xdgRuntimeDir) {
        runtimeDir = xdgRuntimeDir;
    } else {
        // Fallback to /tmp with UID for isolation
        runtimeDir = "/tmp/user-" + std::to_string(getuid());
    }
    
    return runtimeDir;
}

std::string XDGPaths::getSystemConfigDir() {
    return std::string("/usr/local/etc/") + APP_NAME;
}

std::string XDGPaths::getUserConfigDir(bool createIfMissing) {
    std::string configDir;
    const char* xdgConfigHome = getenv("XDG_CONFIG_HOME");
    
    if (xdgConfigHome && *xdgConfigHome) {
        configDir = xdgConfigHome;
    } else {
        std::string home = getHomeDir();
        if (!home.empty()) {
            configDir = home + "/.config";
        }
    }
    
    if (!configDir.empty()) {
        configDir += "/" + APP_NAME;
        
        if (createIfMissing) {
            createDirectories(configDir);
        }
    }
    
    return configDir;
}

std::string XDGPaths::getUserCacheDir(bool createIfMissing) {
    std::string cacheDir;
    const char* xdgCacheHome = getenv("XDG_CACHE_HOME");
    
    if (xdgCacheHome && *xdgCacheHome) {
        cacheDir = xdgCacheHome;
    } else {
        std::string home = getHomeDir();
        if (!home.empty()) {
            cacheDir = home + "/.cache";
        }
    }
    
    if (!cacheDir.empty()) {
        cacheDir += "/" + APP_NAME;
        
        if (createIfMissing) {
            createDirectories(cacheDir);
        }
    }
    
    return cacheDir;
}

bool XDGPaths::createDirectories(const std::string& path, mode_t mode) {
    try {
        if (!fs::exists(path)) {
            bool result = fs::create_directories(path);
            
            // Set permissions on the created directory
            if (result && chmod(path.c_str(), mode) != 0) {
                LOG_WARNING("Failed to set permissions on directory: ", path, " - ", strerror(errno));
            }
            
            return result;
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create directory: ", path, " - ", e.what());
        return false;
    }
}

std::string XDGPaths::getSocketPath(bool createIfMissing) {
    // Always check for explicit environment override first
    const char* socketPathEnv = getenv("CEC_CONTROL_SOCKET");
    if (socketPathEnv && *socketPathEnv) {
        std::string socketPath = socketPathEnv;
        
        if (createIfMissing) {
            // Create parent directory if needed
            std::string parentDir = socketPath.substr(0, socketPath.find_last_of('/'));
            createDirectories(parentDir, 0755);
        }
        
        return socketPath;
    }
    
    // Get the appropriate path based on environment
    RuntimeEnvironment env = detectEnvironment();
    std::string socketDir;
    std::string socketPath;
    
    // Log environment for debugging
    LOG_INFO("Socket path for environment: ", 
             env == RuntimeEnvironment::SYSTEM_SERVICE ? "SYSTEM_SERVICE" :
             env == RuntimeEnvironment::USER_SERVICE ? "USER_SERVICE" : "NORMAL_USER");
    
    switch (env) {
        case RuntimeEnvironment::SYSTEM_SERVICE: {
            // System service socket - always use /run/cec-control/socket
            socketDir = getSystemRuntimeDir();
            LOG_INFO("System runtime directory: ", socketDir);
            socketPath = socketDir + "/" + SOCKET_FILENAME;
            
            if (createIfMissing) {
                createDirectories(socketDir, 0755);
            }
            break;
        }
        
        case RuntimeEnvironment::USER_SERVICE:
        case RuntimeEnvironment::NORMAL_USER: {
            // User-level socket: $XDG_RUNTIME_DIR/cec-control/socket
            socketDir = getUserRuntimeDir() + "/" + APP_NAME;
            socketPath = socketDir + "/" + SOCKET_FILENAME;
            
            if (createIfMissing) {
                createDirectories(socketDir, 0700);
            }
            break;
        }
    }
    
    // For clients, they should check both potential locations
    if (!createIfMissing) {
        // If we're a client and the socket doesn't exist at our expected path,
        // check the system location as a fallback
        if (env != RuntimeEnvironment::SYSTEM_SERVICE && 
            access(socketPath.c_str(), F_OK) != 0) {
            
            std::string systemSocketPath = std::string("/run/") + APP_NAME + "/" + SOCKET_FILENAME;
            if (access(systemSocketPath.c_str(), F_OK) == 0) {
                return systemSocketPath;
            }
        }
    }
    
    return socketPath;
}

std::string XDGPaths::getConfigPath(bool createIfMissing) {
    // First check for override
    const char* configPathEnv = getenv("CEC_CONTROL_CONFIG");
    if (configPathEnv && *configPathEnv) {
        return configPathEnv;
    }
    
    // Get environment-appropriate path
    RuntimeEnvironment env = detectEnvironment();
    std::string configPath;
    
    switch (env) {
        case RuntimeEnvironment::SYSTEM_SERVICE: {
            // System config
            configPath = getSystemConfigDir() + "/" + CONFIG_FILENAME;
            break;
        }
        
        case RuntimeEnvironment::USER_SERVICE:
        case RuntimeEnvironment::NORMAL_USER: {
            // User config
            std::string configDir = getUserConfigDir(createIfMissing);
            if (!configDir.empty()) {
                configPath = configDir + "/" + CONFIG_FILENAME;
                
                // If user config doesn't exist but system config does, first check there
                if (!createIfMissing && access(configPath.c_str(), F_OK) != 0) {
                    std::string systemConfigPath = getSystemConfigDir() + "/" + CONFIG_FILENAME;
                    if (access(systemConfigPath.c_str(), F_OK) == 0) {
                        return systemConfigPath;
                    }
                }
            }
            break;
        }
    }
    
    return configPath;
}

std::string XDGPaths::getLogPath(bool createIfMissing) {
    // First check for override
    const char* logPathEnv = getenv("CEC_CONTROL_LOG");
    if (logPathEnv && *logPathEnv) {
        return logPathEnv;
    }
    
    // Get environment-appropriate path
    RuntimeEnvironment env = detectEnvironment();
    std::string logPath;
    
    switch (env) {
        case RuntimeEnvironment::SYSTEM_SERVICE: {
            // System services log to /var/log by convention
            logPath = "/var/log/" + APP_NAME + "/" + LOG_FILENAME;
            
            if (createIfMissing) {
                createDirectories("/var/log/" + APP_NAME, 0755);
            }
            break;
        }
        
        case RuntimeEnvironment::USER_SERVICE:
        case RuntimeEnvironment::NORMAL_USER: {
            // User logs go to cache dir
            std::string cacheDir = getUserCacheDir(createIfMissing);
            if (!cacheDir.empty()) {
                logPath = cacheDir + "/" + LOG_FILENAME;
            } else {
                // Fallback to /tmp
                logPath = "/tmp/" + APP_NAME + "-" + std::to_string(getuid()) + ".log";
            }
            break;
        }
    }
    
    return logPath;
}

std::string XDGPaths::getRuntimeDir(bool createIfMissing) {
    RuntimeEnvironment env = detectEnvironment();
    std::string runtimeDir;
    
    switch (env) {
        case RuntimeEnvironment::SYSTEM_SERVICE: {
            runtimeDir = getSystemRuntimeDir();
            if (createIfMissing) {
                createDirectories(runtimeDir, 0755);
            }
            break;
        }
        
        case RuntimeEnvironment::USER_SERVICE:
        case RuntimeEnvironment::NORMAL_USER: {
            runtimeDir = getUserRuntimeDir() + "/" + APP_NAME;
            if (createIfMissing) {
                createDirectories(runtimeDir, 0700);
            }
            break;
        }
    }
    
    return runtimeDir;
}

} // namespace cec_control
