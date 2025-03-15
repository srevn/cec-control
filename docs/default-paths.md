# Default Paths Overview

## System Service (when installed via systemd)

  - Config File: /etc/cec-control/config.conf
  - Log File: /var/log/cec-control/daemon.log
  - Socket Path: /run/cec-control/socket
  - Runtime Dir: /run/cec-control

## Normal User Mode

  - Config File: ~/.config/cec-control/config.conf
  - Log File: ~/.cache/cec-control/daemon.log
  - Socket Path: $XDG_RUNTIME_DIR/cec-control/socket (typically /run/user/$UID/cec-control/socket)
  - Runtime Dir: $XDG_RUNTIME_DIR/cec-control (typically /run/user/$UID/cec-control)

# CMake Installation Paths

  When you install using CMake, the installation process follows these rules:

  1. Binary files: Installed to ${CMAKE_INSTALL_PREFIX}/bin/ (default prefix is /usr/local unless changed)
  2. System config file: Installed to /etc/cec-control/config.conf as a template
  3. User config file: Created in ~/.config/cec-control/config.conf if the user installing isn't root
  4. Systemd service file: Installed to the systemd unit directory (usually /lib/systemd/system/)

# Getting Log Files

  1. When running as a system service:
    - The logs are sent to the systemd journal
    - The log file is also written to /var/log/cec-control/daemon.log if that directory exists
  2. When running as a normal user:
    - Logs are written to ~/.cache/cec-control/daemon.log

# Can override any path with environment variables:

  - CEC_CONTROL_CONFIG: Set this to override the config file path
  - CEC_CONTROL_LOG: Set this to override the log file path
  - CEC_CONTROL_SOCKET: Set this to override the socket path
  - CEC_CONTROL_ENVIRONMENT: Set to "system_service" or "user_service" to force a specific environment