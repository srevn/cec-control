#pragma once

#include <string>
#include <sstream>
#include <iostream>
#include <chrono>
#include <fstream>
#include <mutex>

namespace cec_control {

enum class LogLevel {
    DEBUG,
    TRAFFIC,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

class Logger {
public:
    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void setLogLevel(LogLevel level) { m_minLevel = level; }
    void setLogFile(const std::string& filePath);

    template<typename... Args>
    void log(LogLevel level, const Args&... args) {
        if (level < m_minLevel) return;

        std::stringstream ss;
        ss << getCurrentTimeString() << " [" << levelToString(level) << "] ";
        logInternal(ss, args...);
    }

    template<typename... Args>
    void debug(const Args&... args) { log(LogLevel::DEBUG, args...); }

    template<typename... Args>
    void info(const Args&... args) { log(LogLevel::INFO, args...); }

    template<typename... Args>
    void warning(const Args&... args) { log(LogLevel::WARNING, args...); }

    template<typename... Args>
    void error(const Args&... args) { log(LogLevel::ERROR, args...); }

    template<typename... Args>
    void fatal(const Args&... args) { log(LogLevel::FATAL, args...); }

private:
    Logger() : m_minLevel(LogLevel::INFO) {}
    ~Logger() { if (m_fileStream.is_open()) m_fileStream.close(); }
    
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    template<typename T, typename... Args>
    void logInternal(std::stringstream& ss, const T& first, const Args&... args) {
        ss << first;
        logInternal(ss, args...);
    }

    void logInternal(std::stringstream& ss) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::cout << ss.str() << std::endl;
        if (m_fileStream.is_open()) {
            m_fileStream << ss.str() << std::endl;
        }
    }

    std::string getCurrentTimeString() const;
    std::string levelToString(LogLevel level) const;

    LogLevel m_minLevel;
    std::ofstream m_fileStream;
    std::mutex m_mutex;
};

#define LOG_DEBUG(...) cec_control::Logger::getInstance().debug(__VA_ARGS__)
#define LOG_INFO(...) cec_control::Logger::getInstance().info(__VA_ARGS__)
#define LOG_WARNING(...) cec_control::Logger::getInstance().warning(__VA_ARGS__)
#define LOG_ERROR(...) cec_control::Logger::getInstance().error(__VA_ARGS__)
#define LOG_FATAL(...) cec_control::Logger::getInstance().fatal(__VA_ARGS__)

} // namespace cec_control