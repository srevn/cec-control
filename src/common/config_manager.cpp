#include "config_manager.h"
#include "logger.h"

#include <fstream>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace cec_control {

std::unique_ptr<ConfigManager> ConfigManager::s_instance;

ConfigManager::ConfigManager(const std::string& configPath)
    : m_configPath(configPath) {
}

bool ConfigManager::load() {
    std::ifstream file(m_configPath);
    if (!file.is_open()) {
        LOG_WARNING("Could not open configuration file: ", m_configPath);
        LOG_INFO("Using default configuration");
        return false;
    }
    
    std::string line;
    std::string currentSection;
    
    while (std::getline(file, line)) {
        // Trim whitespace from the beginning
        line.erase(line.begin(), std::find_if(line.begin(), line.end(),
            [](unsigned char ch) { return !std::isspace(ch); }));
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Check for section header: [Section]
        if (line[0] == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.size() - 2);
            continue;
        }
        
        // Parse key-value pair: Key = Value
        size_t equalPos = line.find('=');
        if (equalPos != std::string::npos) {
            std::string key = line.substr(0, equalPos);
            std::string value = line.substr(equalPos + 1);
            
            // Trim whitespace from key and value
            key.erase(key.begin(), std::find_if(key.begin(), key.end(),
                [](unsigned char ch) { return !std::isspace(ch); }));
            key.erase(std::find_if(key.rbegin(), key.rend(),
                [](unsigned char ch) { return !std::isspace(ch); }).base(), key.end());
                
            value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                [](unsigned char ch) { return !std::isspace(ch); }));
            value.erase(std::find_if(value.rbegin(), value.rend(),
                [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
            
            // Store key-value in the current section
            m_config[currentSection][key] = value;
        }
    }
    
    LOG_INFO("Loaded configuration from ", m_configPath);
    return true;
}

std::string ConfigManager::getString(const std::string& section, 
                                    const std::string& key,
                                    const std::string& defaultValue) const {
    auto sectionIt = m_config.find(section);
    if (sectionIt == m_config.end()) {
        return defaultValue;
    }
    
    auto keyIt = sectionIt->second.find(key);
    if (keyIt == sectionIt->second.end()) {
        return defaultValue;
    }
    
    return keyIt->second;
}

bool ConfigManager::getBool(const std::string& section,
                           const std::string& key,
                           bool defaultValue) const {
    std::string value = getString(section, key, defaultValue ? "true" : "false");
    
    // Convert to lowercase for case-insensitive comparison
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return std::tolower(c); });
    
    return value == "true" || value == "yes" || value == "1" || value == "on";
}

int ConfigManager::getInt(const std::string& section,
                         const std::string& key,
                         int defaultValue) const {
    std::string value = getString(section, key, "");
    
    if (value.empty()) {
        return defaultValue;
    }
    
    try {
        return std::stoi(value);
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to convert '", value, "' to integer: ", e.what());
        return defaultValue;
    }
}

ConfigManager& ConfigManager::getInstance() {
    if (!s_instance) {
        s_instance = std::make_unique<ConfigManager>();
        s_instance->load();
    }
    return *s_instance;
}

} // namespace cec_control
