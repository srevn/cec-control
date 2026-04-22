#include "system_paths.h"
#include "logger.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cec_control {

// Define static constants
const std::string SystemPaths::APP_NAME = "cec-control";
const std::string SystemPaths::CONFIG_FILENAME = "config.conf";
const std::string SystemPaths::LOG_FILENAME = "daemon.log";
const std::string SystemPaths::SOCKET_FILENAME = "socket";

// Standard system paths
const std::string SystemPaths::SYSTEM_CONFIG_BASE = "/etc";
const std::string SystemPaths::SYSTEM_LOG_BASE = "/var/log";
const std::string SystemPaths::SYSTEM_RUN_BASE = "/run";

std::string SystemPaths::getParentDir(const std::string& path) {
    if (path.empty()) {
        LOG_WARNING("Empty path provided to getParentDir");
        return "";
    }
    
    try {
        return std::filesystem::path(path).parent_path().string();
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
        return (std::filesystem::path(base) / component).string();
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

std::string SystemPaths::getSystemRuntimeDir() {
    const char* runtimeDir = getenv("RUNTIME_DIRECTORY");
    if (!runtimeDir || !*runtimeDir) {
        return joinPath(SYSTEM_RUN_BASE, APP_NAME);
    }

    // systemd's RUNTIME_DIRECTORY is either an absolute path or a directory
    // name relative to /run. The bare app-name case is the expected default;
    // anything else gets a log entry so the deviation is visible.
    if (runtimeDir[0] == '/') {
        return runtimeDir;
    }
    if (std::string_view(runtimeDir) != APP_NAME) {
        LOG_INFO("Using runtime directory from systemd: ", runtimeDir);
    }
    return joinPath(SYSTEM_RUN_BASE, runtimeDir);
}

bool SystemPaths::createDirectories(const std::string& path, mode_t mode) {
    if (path.empty()) {
        LOG_ERROR("Empty path provided to createDirectories");
        return false;
    }
    
    try {
        if (!std::filesystem::exists(path)) {
            bool result = std::filesystem::create_directories(path);
            
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

std::string SystemPaths::getSocketPath() {
    if (const char* envOverride = getenv("CEC_CONTROL_SOCKET");
        envOverride && *envOverride) {
        return envOverride;
    }
    return joinPath(getSystemRuntimeDir(), SOCKET_FILENAME);
}

std::string SystemPaths::getConfigPath() {
    if (const char* envOverride = getenv("CEC_CONTROL_CONFIG");
        envOverride && *envOverride) {
        return envOverride;
    }
    return joinPath(joinPath(SYSTEM_CONFIG_BASE, APP_NAME), CONFIG_FILENAME);
}

std::string SystemPaths::getLogPath() {
    if (const char* envOverride = getenv("CEC_CONTROL_LOG");
        envOverride && *envOverride) {
        return envOverride;
    }
    return joinPath(joinPath(SYSTEM_LOG_BASE, APP_NAME), LOG_FILENAME);
}

bool SystemPaths::ensureParentDirExists(const std::string& path, mode_t mode) {
    std::string parent = getParentDir(path);
    if (parent.empty()) {
        return true;  // No parent component (e.g. relative filename in cwd).
    }
    return createDirectories(parent, mode);
}

} // namespace cec_control