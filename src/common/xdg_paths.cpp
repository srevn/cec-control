#include "xdg_paths.h"
#include "logger.h"

#include <cstdlib>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

namespace cec_control {

// Application name used for directories
const std::string APP_NAME = "cec-control";

// Get user's home directory
std::string getHomeDir() {
    const char* homeDir = getenv("HOME");
    
    if (!homeDir) {
        struct passwd* pwd = getpwuid(getuid());
        if (pwd) {
            homeDir = pwd->pw_dir;
        }
    }
    
    return homeDir ? std::string(homeDir) : "";
}

std::string XDGPaths::getConfigHome(bool createIfMissing) {
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
    
    if (createIfMissing && !configDir.empty()) {
        createDirectories(configDir);
    }
    
    return configDir;
}

std::string XDGPaths::getCacheHome(bool createIfMissing) {
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
    
    if (createIfMissing && !cacheDir.empty()) {
        createDirectories(cacheDir);
    }
    
    return cacheDir;
}

std::string XDGPaths::getRuntimeDir(bool createIfMissing) {
    std::string runtimeDir;
    const char* xdgRuntimeDir = getenv("XDG_RUNTIME_DIR");
    
    if (xdgRuntimeDir && *xdgRuntimeDir) {
        runtimeDir = xdgRuntimeDir;
    } else {
        // Check if we're running under systemd
        const char* notifySocket = getenv("NOTIFY_SOCKET");
        if (notifySocket && *notifySocket) {
            // We're running under systemd, use /run/cec-control as runtime dir
            runtimeDir = "/run";
            
            // Check if we have write permissions to /run
            if (access("/run", W_OK) != 0) {
                LOG_WARNING("No write permissions to /run, falling back to /tmp");
                runtimeDir = "/tmp/" + std::to_string(getuid());
            }
        } else {
            // Not under systemd, use default fallback
            runtimeDir = "/tmp/" + std::to_string(getuid());
        }
    }
    
    if (createIfMissing && !runtimeDir.empty()) {
        createDirectories(runtimeDir);
    }
    
    return runtimeDir;
}

bool XDGPaths::createDirectories(const std::string& path) {
    try {
        if (!fs::exists(path)) {
            return fs::create_directories(path);
        }
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to create directory: ", path, " - ", e.what());
        return false;
    }
}

std::string XDGPaths::getAppConfigDir(bool createIfMissing) {
    std::string configDir = getConfigHome(false);
    if (configDir.empty()) return "";
    
    std::string appConfigDir = configDir + "/" + APP_NAME;
    if (createIfMissing) {
        createDirectories(appConfigDir);
    }
    
    return appConfigDir;
}

std::string XDGPaths::getAppCacheDir(bool createIfMissing) {
    std::string cacheDir = getCacheHome(false);
    if (cacheDir.empty()) return "";
    
    std::string appCacheDir = cacheDir + "/" + APP_NAME;
    if (createIfMissing) {
        createDirectories(appCacheDir);
    }
    
    return appCacheDir;
}

std::string XDGPaths::getAppRuntimeDir(bool createIfMissing) {
    std::string runtimeDir = getRuntimeDir(false);
    if (runtimeDir.empty()) return "";
    
    std::string appRuntimeDir = runtimeDir + "/" + APP_NAME;
    if (createIfMissing) {
        createDirectories(appRuntimeDir);
    }
    
    return appRuntimeDir;
}

std::string XDGPaths::getDefaultConfigPath() {
    // Use only XDG path
    std::string appConfigDir = getAppConfigDir(true);
    return appConfigDir + "/config.conf";
}

std::string XDGPaths::getDefaultLogPath() {
    std::string appCacheDir = getAppCacheDir(true);
    return appCacheDir + "/daemon.log";
}

std::string XDGPaths::getDefaultSocketPath() {
    // First check for a specific override in environment
    const char* socketPathEnv = getenv("CEC_CONTROL_SOCKET");
    if (socketPathEnv && *socketPathEnv) {
        return socketPathEnv;
    }
    
    // Check if we're running as a systemd service
    const char* notifySocket = getenv("NOTIFY_SOCKET");
    if (notifySocket && *notifySocket) {
        // Check if the runtime directory was set by systemd
        const char* runtimeDir = getenv("RUNTIME_DIRECTORY");
        if (runtimeDir && *runtimeDir) {
            std::string systemdPath = std::string("/run/") + runtimeDir + "/socket";
            // Test if this path exists - client might need to use it
            if (access(systemdPath.c_str(), F_OK) == 0) {
                return systemdPath;
            }
        }
        
        // If systemd runtime directory is specified but socket doesn't exist yet,
        // or if runtime directory isn't specified, use the standard runtime dir
        std::string systemdPath = "/run/cec-control/socket";
        createDirectories("/run/cec-control");
        return systemdPath;
    }
    
    // Normal user mode - use XDG runtime dir
    std::string appRuntimeDir = getAppRuntimeDir(true);
    return appRuntimeDir + "/socket";
}

} // namespace cec_control
