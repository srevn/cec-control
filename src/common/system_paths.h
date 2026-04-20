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
     * Get the canonical socket path. Pure query: no filesystem side effects.
     * Honours $CEC_CONTROL_SOCKET; falls back to a path under SYSTEM_RUN_BASE.
     */
    static std::string getSocketPath();

    /**
     * Get the canonical config file path. Pure query: no filesystem side effects.
     * Honours $CEC_CONTROL_CONFIG; falls back to a path under SYSTEM_CONFIG_BASE.
     */
    static std::string getConfigPath();

    /**
     * Get the canonical log file path. Pure query: no filesystem side effects.
     * Honours $CEC_CONTROL_LOG; falls back to a path under SYSTEM_LOG_BASE.
     */
    static std::string getLogPath();

    /**
     * Ensure that the parent directory of @p path exists, creating it (and any
     * intermediate parents) with @p mode if necessary. For daemon-side use:
     * the client must never invoke this, since it may have insufficient
     * privileges to create system directories.
     *
     * @return True if the parent directory exists and is writable on return.
     */
    static bool ensureParentDirExists(const std::string& path, mode_t mode = 0755);

    /**
     * Create directories recursively with the given permissions.
     * @return True if the directory exists on return (created or pre-existing).
     */
    static bool createDirectories(const std::string& path, mode_t mode = 0755);

    /** Returns true if @p path refers to an existing filesystem entry. */
    static bool pathExists(const std::string& path);
};

} // namespace cec_control