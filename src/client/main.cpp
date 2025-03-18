#include "cec_client.h"
#include "../common/logger.h"
#include "../common/xdg_paths.h"

#include <iostream>
#include <cstdlib>

/**
 * Print usage information for the client
 */
void printUsage() {
    std::cout << "CEC Client - Control CEC devices\n"
              << "\n"
              << "Usage: cec-client COMMAND [ARGS...] [OPTIONS]\n"
              << "\n"
              << "Commands:\n"
              << "  volume (up|down|mute) DEVICE_ID   Control volume\n"
              << "  power (on|off) DEVICE_ID          Power device on or off\n"
              << "  source DEVICE_ID SOURCE_ID        Change input source (see SOURCE_ID details below)\n"
              << "  auto-standby (on|off)             Auto-suspend PC when TV powers off\n"
              << "  restart                           Restart CEC adapter\n"
              << "  suspend                           Suspend CEC operations (system sleep)\n"
              << "  resume                            Resume CEC operations (system wake)\n"
              << "  help                              Show this help\n"
              << "\n"
              << "Options:\n"
              << "  --socket-path=PATH                Set path to daemon socket\n"
              << "                                    (default: " << cec_control::XDGPaths::getSocketPath() << ")\n"
              << "  --config=/path/to/config.conf     Set path to config file\n"
              << "                                    (default: " << cec_control::XDGPaths::getConfigPath() << ")\n"
              << "\n"
              << "Environment Variables:\n"
              << "  CEC_CONTROL_SOCKET                Override the default socket path\n"
              << "                                    Use /run/cec-control/socket for system services\n"
              << "\n"
              << "Examples:\n"
              << "  cec-client volume up 5            Increase volume on device 5\n"
              << "  cec-client power on 0             Turn on device 0\n"
              << "  cec-client source 0 4             Switch device 0 to source 4 (HDMI 3)\n"
              << "  cec-client suspend                Prepare system for sleep\n"
              << "  # Connect to system service:\n"
              << "  CEC_CONTROL_SOCKET=/run/cec-control/socket cec-client power on 0\n"
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

int main(int argc, char* argv[]) {
    // Configure minimal logging for client
    cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::ERROR);
    
    try {
        // Create client and process arguments
        cec_control::CECClient client;
        if (!client.processArgs(argc, argv)) {
            printUsage();
            return EXIT_FAILURE;
        }
        
        // Check if help was requested
        if (client.isHelpRequested()) {
            printUsage();
            return EXIT_SUCCESS;
        }
        
        // Execute the client command
        return client.execute();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}