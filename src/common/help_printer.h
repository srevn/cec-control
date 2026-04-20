#pragma once

#include "argument_parser.h"

namespace cec_control {

class HelpPrinter {
public:
    /**
     * Render the help section selected by @p target on stdout.
     * @param programName The program name (typically argv[0]).
     */
    static void printHelp(HelpTarget target, const char* programName);

private:
    static void printGeneralHelp(const char* programName);
    static void printClientHelp(const char* programName);
    static void printDaemonHelp(const char* programName);
};

} // namespace cec_control
