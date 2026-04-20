#include "logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace cec_control {

Logger& Logger::getInstance() noexcept {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (m_fileStream.is_open()) {
        m_fileStream.flush();
        m_fileStream.close();
    }
}

void Logger::configure(const LogConfig& cfg) {
    std::lock_guard<std::mutex> lock(m_mutex);

    m_lowSink  = cfg.lowLevelSink;
    m_highSink = cfg.highLevelSink;
    m_minLevel.store(cfg.minLevel, std::memory_order_relaxed);

    if (m_fileStream.is_open()) {
        m_fileStream.flush();
        m_fileStream.close();
    }

    if (!cfg.filePath.empty()) {
        m_fileStream.open(cfg.filePath, std::ios::out | std::ios::app);
        if (!m_fileStream.is_open()) {
            // Surface the misconfiguration directly. We avoid the LOG_* macros
            // here because the logger is mid-reconfigure and the new state is
            // not yet visible to other threads.
            std::cerr << "logger: failed to open log file: " << cfg.filePath << '\n';
        }
    }
}

void Logger::emit(LogLevel level, const std::string& line) {
    std::lock_guard<std::mutex> lock(m_mutex);

    const LogSink consoleSink = (level >= LogLevel::WARNING) ? m_highSink : m_lowSink;
    if (std::ostream* console = sinkStream(consoleSink)) {
        *console << line << '\n';
        console->flush();
    }
    if (m_fileStream.is_open()) {
        m_fileStream << line << '\n';
        m_fileStream.flush();
    }
}

std::ostream* Logger::sinkStream(LogSink sink) noexcept {
    switch (sink) {
        case LogSink::Stdout: return &std::cout;
        case LogSink::Stderr: return &std::cerr;
        case LogSink::None:   return nullptr;
    }
    return nullptr;
}

std::string Logger::currentTimeString() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto t   = system_clock::to_time_t(now);
    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch() % seconds(1)).count();

    std::tm tm{};
    localtime_r(&t, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms;
    return oss.str();
}

const char* Logger::levelString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::DEBUG:   return "DEBUG";
        case LogLevel::TRAFFIC: return "TRAFFIC";
        case LogLevel::INFO:    return "INFO";
        case LogLevel::WARNING: return "WARNING";
        case LogLevel::ERROR:   return "ERROR";
        case LogLevel::FATAL:   return "FATAL";
    }
    return "UNKNOWN";
}

} // namespace cec_control
