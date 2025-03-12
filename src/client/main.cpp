#include "cec_client.h"
#include "../common/logger.h"

#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    // Configure minimal logging for client
    cec_control::Logger::getInstance().setLogLevel(cec_control::LogLevel::ERROR);
    
    try {
        // Create client and process arguments
        cec_control::CECClient client;
        if (!client.processArgs(argc, argv)) {
            cec_control::CECClient::printUsage();
            return EXIT_FAILURE;
        }
        
        // Execute the client command
        return client.execute();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}