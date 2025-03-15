#pragma once

#include <string>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cec_control {

/**
 * Utility class for handling XDG Base Directory paths
 */
class XDGPaths {
private:
    /**
     * Get the configuration directory according to XDG spec
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Path to the XDG config directory
     */
    static std::string getConfigHome(bool createIfMissing = true);
    
    /**
     * Get the cache directory according to XDG spec
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Path to the XDG cache directory
     */
    static std::string getCacheHome(bool createIfMissing = true);
    
    /**
     * Get the runtime directory according to XDG spec
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Path to the XDG runtime directory
     */
    static std::string getRuntimeDir(bool createIfMissing = true);
    
public:
    /**
     * Create directories recursively
     * @param path The path to create
     * @return True if successful or directory already exists
     */
    static bool createDirectories(const std::string& path);
    
    /**
     * Get application-specific config directory
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Path to app config directory
     */
    static std::string getAppConfigDir(bool createIfMissing = true);
    
    /**
     * Get application-specific cache directory
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Path to app cache directory
     */
    static std::string getAppCacheDir(bool createIfMissing = true);
    
    /**
     * Get application-specific runtime directory
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Path to app runtime directory
     */
    static std::string getAppRuntimeDir(bool createIfMissing = true);
    
    /**
     * Get default config file path
     * @return Full path to the default config file
     */
    static std::string getDefaultConfigPath();
    
    /**
     * Get default log file path
     * @return Full path to the default log file
     */
    static std::string getDefaultLogPath();
    
    /**
     * Get default socket file path
     * @return Full path to the default socket file
     */
    static std::string getDefaultSocketPath();
};

} // namespace cec_control
