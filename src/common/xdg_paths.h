#pragma once

#include <string>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cec_control {

/**
 * Runtime environment detection for proper path handling
 */
enum class RuntimeEnvironment {
    NORMAL_USER,           // Running as normal user
    SYSTEM_SERVICE,        // Running as system service (systemd, etc.)
    USER_SERVICE           // Running as user service (systemd user unit)
};

/**
 * Comprehensive utility class for handling file paths across different runtime environments
 */
class XDGPaths {
public:
    // Application constants - public for consistent use externally
    static const std::string APP_NAME;
    static const std::string CONFIG_FILENAME;
    static const std::string LOG_FILENAME;
    static const std::string SOCKET_FILENAME;
    
private:
    
    // Runtime environment detection
    static RuntimeEnvironment detectEnvironment();
    
    // Private path helpers
    static std::string getHomeDir();
    static std::string getSystemRuntimeDir();
    static std::string getUserRuntimeDir();
    static std::string getSystemConfigDir();
    static std::string getUserConfigDir(bool createIfMissing = true);
    static std::string getUserCacheDir(bool createIfMissing = true);
    
public:
    /**
     * Create directories recursively with appropriate permissions
     * @param path The path to create
     * @param mode The permissions to set (default: 0755)
     * @return True if successful or directory already exists
     */
    static bool createDirectories(const std::string& path, mode_t mode = 0755);
    
    /**
     * Get the current runtime environment
     * @return Detected runtime environment (NORMAL_USER, SYSTEM_SERVICE, USER_SERVICE)
     */
    static RuntimeEnvironment getEnvironment();
    
    /**
     * Get a consistent socket path that works across all environments
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified socket path
     */
    static std::string getSocketPath(bool createIfMissing = true);
    
    /**
     * Get appropriate config file path for current environment
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified config file path
     */
    static std::string getConfigPath(bool createIfMissing = true);
    
    /**
     * Get appropriate log file path for current environment
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified log file path
     */
    static std::string getLogPath(bool createIfMissing = true);
    
    /**
     * Get temporary runtime directory appropriate for current environment
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified runtime directory path
     */
    static std::string getRuntimeDir(bool createIfMissing = true);
};

} // namespace cec_control
