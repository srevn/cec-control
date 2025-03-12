#include "logger.h"

#include <iomanip>
#include <chrono>

namespace cec_control {

void Logger::setLogFile(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_fileStream.is_open()) {
        m_fileStream.close();
    }
    
    m_fileStream.open(filePath, std::ios::out | std::ios::app);
    if (!m_fileStream.is_open()) {
        std::cerr << "Failed to open log file: " << filePath << std::endl;
    }
}

std::string Logger::getCurrentTimeString() const {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    
    // Get milliseconds part
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch() % std::chrono::seconds(1)
    ).count();
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t_now), "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms;
       
    return ss.str();
}

std::string Logger::levelToString(LogLevel level) const {
    switch(level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::TRAFFIC: return "TRAFFIC";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::FATAL:   return "FATAL";
        default:                return "UNKNOWN";
    }
}

} // namespace cec_control