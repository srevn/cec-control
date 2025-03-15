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
        runtimeDir = "/tmp/" + std::to_string(getuid());
    }
    
    if (createIfMissing && !runtimeDir.empty()) {
        createDirectories(runtimeDir);
    }
    
    return runtimeDir;
}

std::string XDGPaths::getDataHome(bool createIfMissing) {
    std::string dataDir;
    const char* xdgDataHome = getenv("XDG_DATA_HOME");
    
    if (xdgDataHome && *xdgDataHome) {
        dataDir = xdgDataHome;
    } else {
        std::string home = getHomeDir();
        if (!home.empty()) {
            dataDir = home + "/.local/share";
        }
    }
    
    if (createIfMissing && !dataDir.empty()) {
        createDirectories(dataDir);
    }
    
    return dataDir;
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

std::string XDGPaths::getAppDataDir(bool createIfMissing) {
    std::string dataDir = getDataHome(false);
    if (dataDir.empty()) return "";
    
    std::string appDataDir = dataDir + "/" + APP_NAME;
    if (createIfMissing) {
        createDirectories(appDataDir);
    }
    
    return appDataDir;
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
    std::string appRuntimeDir = getAppRuntimeDir(true);
    return appRuntimeDir + "/socket";
}

} // namespace cec_control
