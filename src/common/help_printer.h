#pragma once

#include "application_mode.h"

namespace cec_control {

class HelpPrinter {
public:
    /**
     * Print help information based on the application mode
     * @param mode The help context to display
     * @param programName The name of the program (from argv[0])
     */
    static void printHelp(ApplicationMode mode, const char* programName);
    
private:
    /**
     * Print general help showing overview of both client and daemon modes
     * @param programName The name of the program
     */
    static void printGeneralHelp(const char* programName);
    
    /**
     * Print detailed client help with commands and examples
     * @param programName The name of the program
     */
    static void printClientHelp(const char* programName);
    
    /**
     * Print detailed daemon help with configuration options
     * @param programName The name of the program
     */
    static void printDaemonHelp(const char* programName);
};

} // namespace cec_control