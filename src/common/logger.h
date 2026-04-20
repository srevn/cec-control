#pragma once

#include <atomic>
#include <fstream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>

namespace cec_control {

enum class LogLevel : int {
    DEBUG = 0,
    TRAFFIC,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

/**
 * A console destination for log lines. None discards the line for that
 * severity band; the file sink (if configured) still receives it.
 */
enum class LogSink {
    None,
    Stdout,
    Stderr
};

/**
 * Logger configuration. Pass to Logger::configure() to redirect output.
 *
 * Console sinks are split by severity: messages strictly below WARNING go to
 * lowLevelSink (typically stdout); WARNING and above go to highLevelSink
 * (typically stderr). systemd-journald captures both streams when running
 * under a unit, so the split surfaces severity to the journal automatically.
 *
 * filePath, when non-empty, opens a log file in append mode that receives
 * every message at minLevel and above, regardless of console routing.
 */
struct LogConfig {
    LogSink     lowLevelSink  = LogSink::None;
    LogSink     highLevelSink = LogSink::None;
    std::string filePath;
    LogLevel    minLevel      = LogLevel::INFO;
};

/**
 * Process-wide logger. Singleton; safe to call from any thread.
 *
 * The default configuration is silent: no console sinks, no file. Callers
 * that want output must invoke configure() explicitly. This keeps code paths
 * that should never print (help printing, client invocations) from leaking
 * diagnostics onto stdout/stderr.
 */
class Logger {
public:
    static Logger& getInstance() noexcept;

    /** Replace the active configuration. Serialised against in-flight log() calls. */
    void configure(const LogConfig& cfg);

    /**
     * Lock-free level update. Equivalent to configure() that touches only
     * the minLevel field. Useful for raising/lowering verbosity at runtime.
     */
    void setLevel(LogLevel level) noexcept {
        m_minLevel.store(level, std::memory_order_relaxed);
    }

    template <typename... Args>
    void log(LogLevel level, const Args&... args) {
        if (level < m_minLevel.load(std::memory_order_relaxed)) {
            return;
        }
        std::ostringstream oss;
        oss << currentTimeString() << " [" << levelString(level) << "] ";
        ((oss << args), ...);
        emit(level, oss.str());
    }

    template <typename... Args> void debug  (const Args&... a) { log(LogLevel::DEBUG,   a...); }
    template <typename... Args> void traffic(const Args&... a) { log(LogLevel::TRAFFIC, a...); }
    template <typename... Args> void info   (const Args&... a) { log(LogLevel::INFO,    a...); }
    template <typename... Args> void warning(const Args&... a) { log(LogLevel::WARNING, a...); }
    template <typename... Args> void error  (const Args&... a) { log(LogLevel::ERROR,   a...); }
    template <typename... Args> void fatal  (const Args&... a) { log(LogLevel::FATAL,   a...); }

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void emit(LogLevel level, const std::string& line);

    static std::string currentTimeString();
    static const char* levelString(LogLevel level) noexcept;
    static std::ostream* sinkStream(LogSink sink) noexcept;

    std::atomic<LogLevel> m_minLevel{LogLevel::INFO};
    std::mutex m_mutex;
    LogSink m_lowSink  = LogSink::None;
    LogSink m_highSink = LogSink::None;
    std::ofstream m_fileStream;
};

#define LOG_DEBUG(...)   ::cec_control::Logger::getInstance().debug(__VA_ARGS__)
#define LOG_TRAFFIC(...) ::cec_control::Logger::getInstance().traffic(__VA_ARGS__)
#define LOG_INFO(...)    ::cec_control::Logger::getInstance().info(__VA_ARGS__)
#define LOG_WARNING(...) ::cec_control::Logger::getInstance().warning(__VA_ARGS__)
#define LOG_ERROR(...)   ::cec_control::Logger::getInstance().error(__VA_ARGS__)
#define LOG_FATAL(...)   ::cec_control::Logger::getInstance().fatal(__VA_ARGS__)

} // namespace cec_control
