#pragma once

#include <string>
#include <unordered_map>

namespace cec_control {

/**
 * Loads and provides read access to a section/key INI-style configuration
 * file. A regular value type: construct with a path (empty string selects the
 * default from SystemPaths), call load(), then query via getString/getBool/
 * getInt. Non-copyable, non-movable — the expected lifetime is "local to
 * DaemonBootstrap::runDaemon, used to populate the option structs."
 */
class ConfigManager {
public:
    /**
     * @param configPath Path to the configuration file. Empty selects the
     *                   default from SystemPaths::getConfigPath().
     */
    explicit ConfigManager(std::string configPath = "");
    ~ConfigManager() = default;

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    /** Load configuration from file. Returns false when the file is absent. */
    bool load();

    std::string getString(const std::string& section,
                          const std::string& key,
                          const std::string& defaultValue = "") const;

    bool getBool(const std::string& section,
                 const std::string& key,
                 bool defaultValue = false) const;

    int getInt(const std::string& section,
               const std::string& key,
               int defaultValue = 0) const;

    const std::string& getConfigPath() const noexcept { return m_configPath; }

private:
    std::string m_configPath;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_config;

    static void trim(std::string& s);
};

} // namespace cec_control
