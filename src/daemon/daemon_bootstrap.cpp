#include "daemon_bootstrap.h"

#include "../common/config_manager.h"
#include "../common/logger.h"
#include "../common/system_paths.h"
#include "../common/systemd_notify.h"
#include "app_config.h"
#include "cec_daemon.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
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

    setupProcess();

    LOG_INFO("Running with PID: ", getpid());

    try {
        CECDaemon daemon(std::move(config));

        if (!daemon.start()) {
            LOG_FATAL("Failed to start CEC daemon");
            return EXIT_FAILURE;
        }

        // Announce readiness to the service manager only after every
        // subsystem is wired and its fds are registered. For Type=notify
        // units this is what transitions the unit from activating to
        // active and releases After= dependents; a no-op otherwise.
        SystemdNotify::ready();

        LOG_INFO("CEC daemon initialized successfully, starting main loop");
        daemon.run();

        // run() returns either on a clean signal-driven stop (exit
        // status EXIT_SUCCESS) or after a subsystem latched an
        // unrecoverable condition through requestUnrecoverableShutdown
        // (exit status EXIT_FAILURE). Propagate verbatim so a service
        // manager (systemd's Restart=on-failure) can distinguish the
        // two and restart on the failure path.
        const int status = daemon.exitStatus();
        if (status == EXIT_SUCCESS) {
            LOG_INFO("CEC daemon exited normally");
        } else {
            LOG_ERROR("CEC daemon exited with failure status ", status);
        }
        return status;
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

void DaemonBootstrap::setupProcess() {
    // Close and replace stdin with /dev/null so the daemon never reads
    // from an inherited controlling terminal. systemd's default
    // StandardInput=null already satisfies this under a unit; the
    // redirect here is the equivalent guarantee for non-supervised
    // manual invocations.
    close(STDIN_FILENO);
    const int null = open("/dev/null", O_RDWR);
    if (null < 0) {
        LOG_WARNING("Failed to open /dev/null: ", std::strerror(errno));
        return;
    }
    if (null != STDIN_FILENO) {
        dup2(null, STDIN_FILENO);
        close(null);
    }
}

void DaemonBootstrap::setupLogging(const RunDaemon& action,
                                    const std::string& logFile) {
    // Daemon logging routes by severity:
    //   - INFO/DEBUG/TRAFFIC -> stdout (journald captures as PRIORITY=info)
    //   - WARNING/ERROR/FATAL -> stderr (journald captures as PRIORITY=err)
    // The file sink mirrors everything at the configured threshold so an
    // operator (or a deployment without journald) still has a durable
    // log of the daemon's activity.
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
