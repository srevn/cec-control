#pragma once

#include <string>

namespace cec_control {

enum class ApplicationMode {
    CLIENT,
    DAEMON,
    HELP_GENERAL,
    HELP_CLIENT,
    HELP_DAEMON
};

class ModeDetector {
public:
    /**
     * Detect the application mode based on command line arguments
     * @param argc Argument count
     * @param argv Argument vector
     * @return Detected application mode
     */
    static ApplicationMode detectMode(int argc, char* argv[]);
    
private:
    static bool isClientCommand(const std::string& arg);
    static bool isDaemonOption(const std::string& arg);
};

} // namespace cec_control