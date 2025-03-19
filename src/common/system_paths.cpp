#include "system_paths.h"
#include "logger.h"

#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace cec_control {

// Define static constants
const std::string SystemPaths::APP_NAME = "cec-control";
const std::string SystemPaths::CONFIG_FILENAME = "config.conf";
const std::string SystemPaths::LOG_FILENAME = "daemon.log";
const std::string SystemPaths::SOCKET_FILENAME = "socket";

// Standard system paths
const std::string SystemPaths::SYSTEM_CONFIG_BASE = "/usr/local/etc";
const std::string SystemPaths::SYSTEM_LOG_BASE = "/var/log";
const std::string SystemPaths::SYSTEM_RUN_BASE = "/run";

std::string SystemPaths::getParentDir(const std::string& path) {
    if (path.empty()) {
        LOG_WARNING("Empty path provided to getParentDir");
        return "";
    }
    
    try {
        return fs::path(path).parent_path().string();
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to get parent directory for path: ", path, " - ", e.what());
        // Fallback to find_last_of approach if filesystem throws
        size_t lastSlash = path.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return path.substr(0, lastSlash);
        }
        return "";
    }
}

std::string SystemPaths::joinPath(const std::string& base, const std::string& component) {
    if (base.empty()) {
        return component;
    }
    
    try {
        // Use filesystem path for proper path joining with separators
        return (fs::path(base) / component).string();
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to join paths: ", base, " and ", component, " - ", e.what());
        // Fallback to simple concatenation
        if (base.back() == '/') {
            return base + component;
        } else {
            return base + "/" + component;
        }
    }
}

bool SystemPaths::pathExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    
    try {
        return fs::exists(path);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to check if path exists: ", path, " - ", e.what());
        return false;
    }
}

std::string SystemPaths::getSystemRuntimeDir() {
    // Check if environment has a specific runtime dir from systemd
    const char* runtimeDir = getenv("RUNTIME_DIRECTORY");
    if (runtimeDir && *runtimeDir) {
        // The systemd runtime directory variable already contains the full path under /run
        if (strncmp(runtimeDir, APP_NAME.c_str(), APP_NAME.length()) == 0) {
            // Simple directory name, add /run/
            return joinPath(SYSTEM_RUN_BASE, runtimeDir);
        } else if (runtimeDir[0] == '/') {
            // Absolute path, use as is
            return runtimeDir;
        } else {
            // Relative path but not just our app name
            LOG_INFO("Using runtime directory from systemd: ", runtimeDir);
            return joinPath(SYSTEM_RUN_BASE, runtimeDir);
        }
    }
    
    // Default system runtime directory
    return joinPath(SYSTEM_RUN_BASE, APP_NAME);
}

bool SystemPaths::createDirectories(const std::string& path, mode_t mode) {
    if (path.empty()) {
        LOG_ERROR("Empty path provided to createDirectories");
        return false;
    }
    
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

std::string SystemPaths::getSocketPath(bool createIfMissing) {
    // Always check for explicit environment override first
    const char* socketPathEnv = getenv("CEC_CONTROL_SOCKET");
    if (socketPathEnv && *socketPathEnv) {
        std::string socketPath = socketPathEnv;
        
        if (createIfMissing && !socketPath.empty()) {
            // Create parent directory if needed
            std::string parentDir = getParentDir(socketPath);
            if (!parentDir.empty()) {
                createDirectories(parentDir, 0755);
            }
        }
        
        return socketPath;
    }
    
    // System service socket - always use /run/cec-control/socket
    std::string socketDir = getSystemRuntimeDir();
    LOG_INFO("System runtime directory: ", socketDir);
    std::string socketPath = joinPath(socketDir, SOCKET_FILENAME);
    
    if (createIfMissing && !socketDir.empty()) {
        createDirectories(socketDir, 0755);
    }
    
    return socketPath;
}

std::string SystemPaths::getConfigPath(bool createIfMissing) {
    // First check for override
    const char* configPathEnv = getenv("CEC_CONTROL_CONFIG");
    if (configPathEnv && *configPathEnv) {
        return configPathEnv;
    }
    
    // System config
    std::string configDir = joinPath(SYSTEM_CONFIG_BASE, APP_NAME);
    std::string configPath = joinPath(configDir, CONFIG_FILENAME);
    
    // Create config directory if needed
    if (createIfMissing && !configDir.empty()) {
        createDirectories(configDir, 0755);
    }
    
    return configPath;
}

std::string SystemPaths::getLogPath(bool createIfMissing) {
    // First check for override
    const char* logPathEnv = getenv("CEC_CONTROL_LOG");
    if (logPathEnv && *logPathEnv) {
        return logPathEnv;
    }
    
    // System services log to /var/log by convention
    std::string logDir = joinPath(SYSTEM_LOG_BASE, APP_NAME);
    std::string logPath = joinPath(logDir, LOG_FILENAME);
    
    if (createIfMissing && !logDir.empty()) {
        createDirectories(logDir, 0755);
    }
    
    return logPath;
}

std::string SystemPaths::getRuntimeDir(bool createIfMissing) {
    std::string runtimeDir = getSystemRuntimeDir();
    if (createIfMissing && !runtimeDir.empty()) {
        createDirectories(runtimeDir, 0755);
    }
    
    return runtimeDir;
}

} // namespace cec_control