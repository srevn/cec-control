#include "daemon_bootstrap.h"

#include "../common/config_manager.h"
#include "../common/logger.h"
#include "../common/system_paths.h"
#include "../common/systemd_env.h"
#include "app_config.h"
#include "cec_daemon.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace cec_control {

int DaemonBootstrap::runDaemon(const RunDaemon& action) {
    // Resolve any unset path knobs to their system defaults exactly once,
    // here at the top, so the rest of the bootstrap doesn't need to carry
    // around "empty means default" branching.
    const std::string logFile    = action.logFile.empty()
                                 ? SystemPaths::getLogPath()
                                 : action.logFile;

    const std::string socketPath = SystemPaths::getSocketPath();

    // Provision system directories the daemon will write into. Done before
    // logging is set up, since the logger opens the log file by path.
    SystemPaths::ensureParentDirExists(logFile);
    SystemPaths::ensureParentDirExists(socketPath);

    setupLogging(action, logFile);

    // Configuration is a local value; once we've extracted the AppConfig
    // snapshot it falls out of scope. No ambient/singleton access after
    // this point.
    ConfigManager configManager(action.configFile);
    if (!configManager.load()) {
        LOG_WARNING("Failed to load configuration file, using defaults");
    }

    AppConfig config = loadAppConfig(configManager);
    logAppConfig(config);

    // Setup the process (daemonization, service mode, etc.). A
    // Should-Return outcome unwinds the stack-owned configuration
    // objects above before main() returns, preserving RAII — the
    // old exit() path skipped them.
    switch (setupProcess(/*runAsDaemon=*/!action.foreground)) {
    case BootstrapPhase::ShouldReturnSuccess:
        return EXIT_SUCCESS;
    case BootstrapPhase::ShouldReturnFailure:
        LOG_FATAL("Failed to setup daemon process");
        return EXIT_FAILURE;
    case BootstrapPhase::ShouldRun:
        break;
    }

    LOG_INFO("Running with PID: ", getpid(), " in system service mode");

    try {
        CECDaemon daemon(std::move(config));

        if (!daemon.start()) {
            LOG_FATAL("Failed to start CEC daemon");
            return EXIT_FAILURE;
        }

        LOG_INFO("CEC daemon initialized successfully, starting main loop");
        daemon.run();

        LOG_INFO("CEC daemon exited normally");
        return EXIT_SUCCESS;
    }
    catch (const std::exception& e) {
        LOG_FATAL("Exception in CEC daemon: ", e.what());
        return EXIT_FAILURE;
    }
    catch (...) {
        LOG_FATAL("Unknown exception in CEC daemon");
        return EXIT_FAILURE;
    }
}

DaemonBootstrap::BootstrapPhase
DaemonBootstrap::setupProcess(bool runAsDaemon) {
    const bool runningUnderSystemd = SystemdEnv::isUnderSystemd();

    if (runAsDaemon && !runningUnderSystemd) {
        LOG_INFO("Running as normal executable, will daemonize");

        switch (daemonize()) {
        case BootstrapPhase::ShouldReturnSuccess:
            // Fork parent — daemonize() already logged the exit
            // banner for the first-fork parent; the second-fork
            // parent is intentionally silent (it runs briefly after
            // setsid with no operator-visible role).
            return BootstrapPhase::ShouldReturnSuccess;
        case BootstrapPhase::ShouldReturnFailure:
            return BootstrapPhase::ShouldReturnFailure;
        case BootstrapPhase::ShouldRun:
            break;
        }

        LOG_INFO("Daemon process started with PID: ", getpid());
    } else {
        if (runningUnderSystemd) {
            LOG_INFO("Running under systemd, not daemonizing");
        } else {
            LOG_INFO("Running in foreground mode");
        }

        // When running as service or in foreground, just redirect stdin.
        close(STDIN_FILENO);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null > STDIN_FILENO) {
                close(null);
            }
        } else {
            LOG_WARNING("Failed to open /dev/null: ", strerror(errno));
        }
    }

    return BootstrapPhase::ShouldRun;
}

DaemonBootstrap::BootstrapPhase DaemonBootstrap::daemonize() {
    umask(022);

    // First fork: the parent process returns to runDaemon so the user's
    // shell prompt comes back; the child continues into setsid.
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork daemon process" << std::endl;
        return BootstrapPhase::ShouldReturnFailure;
    }
    if (pid > 0) {
        // First-fork parent. Log here so the message fires once and
        // only for this process; the second-fork parent below stays
        // silent.
        LOG_INFO("Parent process exiting, daemon started");
        return BootstrapPhase::ShouldReturnSuccess;
    }

    // First-fork child: become session leader so the subsequent fork
    // truly detaches from any controlling terminal.
    if (setsid() < 0) {
        std::cerr << "Failed to create new session" << std::endl;
        return BootstrapPhase::ShouldReturnFailure;
    }

    // Second fork: prevents the daemon from ever reacquiring a
    // controlling terminal (it is no longer a session leader).
    pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork daemon process (2nd fork)" << std::endl;
        return BootstrapPhase::ShouldReturnFailure;
    }
    if (pid > 0) {
        // Second-fork parent. No log: this process runs for
        // microseconds after setsid and has no operator-visible role.
        return BootstrapPhase::ShouldReturnSuccess;
    }

    // True daemon child. Strip down to a neutral process environment.
    chdir("/");

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    return BootstrapPhase::ShouldRun;
}

void DaemonBootstrap::setupLogging(const RunDaemon& action,
                                    const std::string& logFile) {
    // Daemon logging routes by severity:
    //   - INFO/DEBUG/TRAFFIC -> stdout (journald captures as PRIORITY=info)
    //   - WARNING/ERROR/FATAL -> stderr (journald captures as PRIORITY=err)
    // The file sink mirrors everything at the configured threshold so a
    // foreground operator (or a deployment without journald) still has a
    // durable log even if the standard streams are redirected to /dev/null
    // by daemonize().
    LogConfig cfg;
    cfg.lowLevelSink  = LogSink::Stdout;
    cfg.highLevelSink = LogSink::Stderr;
    cfg.filePath      = logFile;
    cfg.minLevel      = action.verbose ? LogLevel::DEBUG : LogLevel::INFO;

    Logger::getInstance().configure(cfg);

    LOG_INFO("Logging initialised; file=", logFile,
             ", level=", action.verbose ? "DEBUG" : "INFO");
}

} // namespace cec_control
