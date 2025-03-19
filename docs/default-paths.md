# Default Paths Overview

## System Service (when installed via systemd)

  - Config File: /usr/local/etc/cec-control/config.conf
  - Log File: /var/log/cec-control/daemon.log
  - Socket Path: /run/cec-control/socket
  - Runtime Dir: /run/cec-control

# CMake Installation Paths

  When you install using CMake, the installation process follows these rules:

  1. Binary files: Installed to ${CMAKE_INSTALL_PREFIX}/bin/ (default prefix is /usr/local unless changed)
  2. System config file: Installed to /usr/local/etc/cec-control/config.conf as a template
  3. Systemd service file: Installed to the systemd unit directory (usually /lib/systemd/system/)

# Getting Log Files

  1. When running as a system service:
    - The logs are sent to the systemd journal
    - The log file is also written to /var/log/cec-control/daemon.log if that directory exists

# Can override any path with environment variables:

  - CEC_CONTROL_CONFIG: Set this to override the config file path
  - CEC_CONTROL_LOG: Set this to override the log file path
  - CEC_CONTROL_SOCKET: Set this to override the socket path