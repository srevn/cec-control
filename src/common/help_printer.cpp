#include "help_printer.h"
#include "system_paths.h"
#include <iostream>

namespace cec_control {

void HelpPrinter::printHelp(ApplicationMode mode, const char* programName) {
    switch (mode) {
        case ApplicationMode::HELP_GENERAL:
            printGeneralHelp(programName);
            break;
        case ApplicationMode::HELP_CLIENT:
            printClientHelp(programName);
            break;
        case ApplicationMode::HELP_DAEMON:
            printDaemonHelp(programName);
            break;
        default:
            printGeneralHelp(programName);
            break;
    }
}

void HelpPrinter::printGeneralHelp(const char* programName) {
    std::cout << "CEC Control - HDMI-CEC device management\n"
              << "\n"
              << "USAGE:\n"
              << "  " << programName << " [COMMAND] [ARGS...] [OPTIONS]    # Client mode\n"
              << "  " << programName << " --daemon [OPTIONS]               # Daemon mode\n"
              << "\n"
              << "CLIENT COMMANDS:\n"
              << "  volume (up|down|mute) DEVICE_ID          Control volume\n"
              << "  power (on|off) DEVICE_ID                 Power device on or off\n"
              << "  source DEVICE_ID SOURCE_ID               Change input source\n"
              << "  auto-standby (on|off)                    Enable/disable automatic standby\n"
              << "  restart                                  Restart CEC adapter\n"
              << "  suspend                                  Prepare for system sleep\n"
              << "  resume                                   Restore after system wake\n"
              << "\n"
              << "DAEMON OPTIONS:\n"
              << "  -d, --daemon                             Run in daemon mode\n"
              << "  -v, --verbose                            Enable verbose logging\n"
              << "  -f, --foreground                         Run in foreground\n"
              << "  -l, --log FILE                           Set log file path\n"
              << "  -c, --config FILE                        Set configuration file\n"
              << "\n"
              << "DETAILED HELP:\n"
              << "  " << programName << " help client        Show client command reference\n"
              << "  " << programName << " help daemon        Show daemon configuration reference\n"
              << "\n"
              << "EXAMPLES:\n"
              << "  " << programName << " power on 0         Turn on TV\n"
              << "  " << programName << " --daemon           Run daemon\n"
              << std::endl;
}

void HelpPrinter::printClientHelp(const char* programName) {
    std::cout << "CEC Client - Control CEC devices\n"
              << "\n"
              << "USAGE:\n"
              << "  " << programName << " COMMAND [ARGS...] [OPTIONS]\n"
              << "\n"
              << "COMMANDS:\n"
              << "  volume (up|down|mute) DEVICE_ID          Control volume\n"
              << "  power (on|off) DEVICE_ID                 Power device on or off\n"
              << "  source DEVICE_ID SOURCE_ID               Change input source (use DEVICE_ID 0 for TV)\n"
              << "  auto-standby (on|off)                    Enable/disable automatic PC suspend when TV powers off\n"
              << "  restart                                  Restart CEC adapter\n"
              << "  suspend                                  Prepare for system sleep (powers off configured devices)\n"
              << "  resume                                   Restore after system wake (powers on configured devices)\n"
              << "  help                                     Show general help\n"
              << "\n"
              << "OPTIONS:\n"
              << "  --socket-path=PATH                       Set daemon socket path\n"
              << "                                           (default: " << SystemPaths::getSocketPath() << ")\n"
              << "  --config=PATH                            Set configuration file path\n"
              << "                                           (default: " << SystemPaths::getConfigPath() << ")\n"
              << "\n"
              << "ENVIRONMENT:\n"
              << "  CEC_CONTROL_SOCKET                       Override socket path for system service\n"
              << "                                           (use /run/cec-control/socket)\n"
              << "\n"
              << "EXAMPLES:\n"
              << "  " << programName << " volume up 5        Increase volume on device 5\n"
              << "  " << programName << " power on 0         Turn on TV (device 0)\n"
              << "  " << programName << " source 0 4         Switch TV to HDMI 3\n"
              << "  " << programName << " suspend            Prepare for system sleep\n"
              << "  CEC_CONTROL_SOCKET=/run/cec-control/socket " << programName << " power on 0\n"
              << "\n"
              << "DEVICE IDs (CEC logical addresses):\n"
              << "  0  - TV                                  5  - Audio System\n"
              << "  1  - Recording Device 1                  4  - Playback Device 1\n"
              << "\n"
              << "SOURCE IDs (input sources):\n"
              << "  0  - General AV input                    3  - HDMI 2\n"
              << "  1  - Audio input                         4  - HDMI 3\n"
              << "  2  - HDMI 1                              5  - HDMI 4\n"
              << std::endl;
}

void HelpPrinter::printDaemonHelp(const char* programName) {
    std::cout << "CEC Daemon - Background service for CEC device management\n"
              << "\n"
              << "USAGE:\n"
              << "  " << programName << " --daemon [OPTIONS]\n"
              << "\n"
              << "OPTIONS:\n"
              << "  -d, --daemon                             Run in daemon mode (default)\n"
              << "  -v, --verbose                            Enable verbose logging\n"
              << "  -f, --foreground                         Run in foreground (don't daemonize)\n"
              << "  -l, --log FILE                           Set log file path\n"
              << "                                           (default: " << SystemPaths::getLogPath() << ")\n"
              << "  -c, --config FILE                        Set configuration file path\n"
              << "                                           (default: " << SystemPaths::getConfigPath() << ")\n"
              << "  -h, --help                               Show this help message\n"
              << "\n"
              << "EXAMPLES:\n"
              << "  " << programName << " --daemon                         Run daemon in background\n"
              << "  " << programName << " --daemon --verbose --foreground  Run with verbose logging in foreground\n"
              << "  " << programName << " -d -c /path/to/config.conf       Run with custom configuration\n"
              << std::endl;
}

} // namespace cec_control
