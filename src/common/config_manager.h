#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace cec_control {

/**
 * Manages loading and accessing configuration settings
 */
class ConfigManager {
public:
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;
    ~ConfigManager() = default;

    /**
     * Load configuration from file
     * @return true if successful, false otherwise
     */
    bool load();
    
    /**
     * Get string value from configuration
     * @param section The configuration section
     * @param key The configuration key
     * @param defaultValue Default value if key not found
     * @return The configuration value
     */
    std::string getString(const std::string& section, 
                         const std::string& key,
                         const std::string& defaultValue = "") const;
    
    /**
     * Get boolean value from configuration
     * @param section The configuration section
     * @param key The configuration key
     * @param defaultValue Default value if key not found
     * @return The configuration value
     */
    bool getBool(const std::string& section,
                const std::string& key,
                bool defaultValue = false) const;
    
    /**
     * Get integer value from configuration
     * @param section The configuration section
     * @param key The configuration key
     * @param defaultValue Default value if key not found
     * @return The configuration value
     */
    int getInt(const std::string& section,
              const std::string& key,
              int defaultValue = 0) const;

    /**
     * Get singleton instance. First call initializes with configPath.
     * @param configPath Path to the configuration file (used only on first call)
     * @return The singleton instance
     */
    static ConfigManager& getInstance(const std::string& configPath = "");
    
    /**
     * Get the path to the configuration file being used
     * @return Path to the configuration file
     */
    std::string getConfigPath() const { return m_configPath; }
    
private:
    explicit ConfigManager(const std::string& configPath);

    std::string m_configPath;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_config;

    static void trim(std::string& s);
};

} // namespace cec_control
