#pragma once

#include <string>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <filesystem>

namespace cec_control {

/**
 * Utility class for handling system file paths
 */
class SystemPaths {
public:
    // Application constants - public for consistent use externally
    static const std::string APP_NAME;
    static const std::string CONFIG_FILENAME;
    static const std::string LOG_FILENAME;
    static const std::string SOCKET_FILENAME;
    
    // Standard system paths
    static const std::string SYSTEM_CONFIG_BASE;
    static const std::string SYSTEM_LOG_BASE;
    static const std::string SYSTEM_RUN_BASE;
    
private:
    // System path helpers
    static std::string getSystemRuntimeDir();
    
    /**
     * Gets the parent directory of a path
     * @param path The path to get the parent directory of
     * @return The parent directory path
     */
    static std::string getParentDir(const std::string& path);
    
    /**
     * Joins path components safely
     * @param base The base path
     * @param component The component to append
     * @return The joined path
     */
    static std::string joinPath(const std::string& base, const std::string& component);
    
public:
    /**
     * Create directories recursively with appropriate permissions
     * @param path The path to create
     * @param mode The permissions to set (default: 0755)
     * @return True if successful or directory already exists
     */
    static bool createDirectories(const std::string& path, mode_t mode = 0755);
    
    /**
     * Get a consistent socket path
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified socket path
     */
    static std::string getSocketPath(bool createIfMissing = true);
    
    /**
     * Get the appropriate config file path
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified config file path
     */
    static std::string getConfigPath(bool createIfMissing = true);
    
    /**
     * Get the appropriate log file path
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified log file path
     */
    static std::string getLogPath(bool createIfMissing = true);
    
    /**
     * Get the runtime directory
     * @param createIfMissing Create the directory if it doesn't exist
     * @return Fully qualified runtime directory path
     */
    static std::string getRuntimeDir(bool createIfMissing = true);
    
    /**
     * Check if a path exists
     * @param path The path to check
     * @return True if the path exists
     */
    static bool pathExists(const std::string& path);
};

} // namespace cec_control