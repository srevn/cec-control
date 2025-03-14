#pragma once

#include <string>
#include <unordered_map>
#include <memory>

namespace cec_control {

/**
 * Manages loading and accessing configuration settings
 */
class ConfigManager {
public:
    /**
     * Create a new config manager
     * @param configPath Path to the configuration file
     */
    ConfigManager(const std::string& configPath = "/etc/cec-control.conf");
    
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
     * Get singleton instance
     * @return The singleton instance
     */
    static ConfigManager& getInstance();
    
private:
    std::string m_configPath;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_config;
    
    static std::unique_ptr<ConfigManager> s_instance;
};

} // namespace cec_control
