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
              << "  " << programName << " [CLIENT_COMMAND] [ARGS...] [OPTIONS]     # Client mode\n"
              << "  " << programName << " --daemon [OPTIONS]                       # Daemon mode\n"
              << "\n"
              << "CLIENT COMMANDS:\n"
              << "  volume (up|down|mute) DEVICE_ID        Control volume\n"
              << "  power (on|off) DEVICE_ID               Power device on or off\n"
              << "  source DEVICE_ID SOURCE_ID             Change input source\n"
              << "  auto-standby (on|off)                  Enable/disable automatic standby\n"
              << "  restart                                Restart CEC adapter\n"
              << "  suspend                                Prepare for system sleep\n"
              << "  resume                                 Restore after system wake\n"
              << "\n"
              << "DAEMON OPTIONS:\n"
              << "  --daemon, -d                           Run in daemon mode\n"
              << "  --verbose, -v                          Enable verbose logging\n"
              << "  --foreground, -f                       Run in foreground\n"
              << "  --log FILE, -l FILE                    Set log file path\n"
              << "  --config FILE, -c FILE                 Set configuration file\n"
              << "\n"
              << "DETAILED HELP:\n"
              << "  " << programName << " help client       Show detailed client command reference\n"
              << "  " << programName << " help daemon       Show detailed daemon configuration reference\n"
              << "\n"
              << "EXAMPLES:\n"
              << "  " << programName << " power on 0        Turn on TV\n"
              << "  " << programName << " -d -v             Run daemon with verbose logging\n"
              << std::endl;
}

void HelpPrinter::printClientHelp(const char* programName) {
    std::cout << "CEC Client - Control CEC devices\n"
              << "\n"
              << "Usage: " << programName << " COMMAND [ARGS...] [OPTIONS]\n"
              << "\n"
              << "Commands:\n"
              << "  volume (up|down|mute) DEVICE_ID   Control volume\n"
              << "  power (on|off) DEVICE_ID          Power device on or off\n"
              << "  source DEVICE_ID SOURCE_ID        Change input source (DEVICE_ID should typically be 0/TV)\n"
              << "  auto-standby (on|off)             Enable/disable automatic PC suspend when TV powers off\n"
              << "  restart                           Restart CEC adapter\n"
              << "  suspend                           Prepare CEC for system sleep (powers off configured devices)\n"
              << "  resume                            Restore CEC after system wake (powers on configured devices)\n"
              << "  help                              Show this help\n"
              << "\n"
              << "Options:\n"
              << "  --socket-path=PATH                Set path to daemon socket\n"
              << "                                    (default: " << SystemPaths::getSocketPath() << ")\n"
              << "  --config=/path/to/config.conf     Set path to config file\n"
              << "                                    (default: " << SystemPaths::getConfigPath() << ")\n"
              << "\n"
              << "Environment Variables:\n"
              << "  CEC_CONTROL_SOCKET                Override the default socket path\n"
              << "                                    Use /run/cec-control/socket for system services\n"
              << "\n"
              << "Examples:\n"
              << "  " << programName << " volume up 5            Increase volume on device 5\n"
              << "  " << programName << " power on 0             Turn on device 0\n"
              << "  " << programName << " source 0 4             Switch device 0 to source 4 (HDMI 3)\n"
              << "  " << programName << " suspend                Prepare system for sleep\n"
              << "  # Connect to system service:\n"
              << "  CEC_CONTROL_SOCKET=/run/cec-control/socket " << programName << " power on 0\n"
              << "\n"
              << "SOURCE_ID mapping:\n"
              << "  0   - General AV input\n"
              << "  1   - Audio input\n"
              << "  2   - HDMI 1\n"
              << "  3   - HDMI 2\n"
              << "  4   - HDMI 3\n"
              << "  5   - HDMI 4\n"
              << "\n"
              << "DEVICE_ID typically ranges from 0-15 and maps to CEC logical addresses:\n"
              << "  0   - TV\n"
              << "  1   - Recording Device 1\n"
              << "  4   - Playback Device 1 (e.g., DVD/Blu-ray player)\n"
              << "  5   - Audio System\n"
              << std::endl;
}

void HelpPrinter::printDaemonHelp(const char* programName) {
    std::cout << "Usage: " << programName << " [OPTIONS]\n"
              << "Options:\n"
              << "  -v, --verbose           Enable verbose logging (to console and log file)\n"
              << "  -f, --foreground        Run in foreground (don't daemonize)\n"
              << "  -l, --log FILE          Log to FILE (default: " << SystemPaths::getLogPath() << ")\n"
              << "  -c, --config FILE       Set configuration file path\n"
              << "                          (default: " << SystemPaths::getConfigPath() << ")\n"
              << "  -h, --help              Show this help message\n"
              << "  -d, --daemon            Run in daemon mode (default)\n"
              << std::endl;
}

} // namespace cec_control