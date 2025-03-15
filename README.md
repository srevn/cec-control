# CEC Control

A comprehensive application for controlling HDMI devices over the CEC (Consumer Electronics Control) protocol. CEC Control provides both a daemon service for continuous HDMI device management and a command-line client for sending commands to your devices.

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![Version](https://img.shields.io/badge/version-1.0.0-green.svg)

## Features

- **Power Management**: Turn devices on/off
- **Volume Control**: Adjust volume, mute, and unmute
- **Input Source Switching**: Change inputs on TVs and receivers
- **System Integration**: Automatic handling of system sleep/wake events
- **Daemon/Client Architecture**: Run as a background service with command-line control
- **XDG Compatible**: Follows the XDG Base Directory Specification
- **Systemd Integration**: Run as a user or system service

## Requirements

- Linux-based operating system
- libcec 4.0.0 or newer
- D-Bus 1.6 or newer
- CMake 3.10 or newer
- C++ 17 compatible compiler

## Installation

### From Source

1. Install the dependencies:
   ```bash
   # Ubuntu/Debian
   sudo apt install build-essential cmake libcec-dev libdbus-1-dev
   
   # Fedora
   sudo dnf install cmake libcec-devel dbus-devel
   
   # Arch Linux
   sudo pacman -S cmake libcec dbus
   ```

2. Clone the repository:
   ```bash
   git clone https://github.com/srevn/cec-control.git
   cd cec-control
   ```

3. Build & Install
   ```bash
   # For normal user installation
   cmake -B build -S .
   cmake --build build
   cmake --install build

   # For system-wide installation
   cmake -B build -S . -DSYSTEM_WIDE_INSTALL=ON
   cmake --build build
   sudo cmake --install build
   ```

### Systemd Service Setup

After installation, you can enable the CEC daemon service:

For user-level service:
```bash
systemctl --user enable cec-daemon.service
systemctl --user start cec-daemon.service
```

For system-level service:
```bash
sudo systemctl enable cec-daemon.service
sudo systemctl start cec-daemon.service
```

## Usage

### Basic Commands

```bash
# Power on the TV (logical address 0)
cec-client power on 0

# Power off the AV Receiver (logical address 5)
cec-client power off 5

# Turn up the volume
cec-client volume up 5

# Mute the audio
cec-client volume mute 5

# Change input source to HDMI 1
cec-client source 0 2

# Restart the CEC adapter
cec-client restart

# Prepare for system sleep
cec-client suspend

# Resume from sleep
cec-client resume
```

### Command Reference

```
Usage: cec-client COMMAND [ARGS...] [OPTIONS]

Commands:
  volume (up|down|mute) DEVICE_ID   Control volume
  power (on|off) DEVICE_ID          Power device on or off
  source DEVICE_ID SOURCE_ID        Change input source
  restart                           Restart CEC adapter
  suspend                           Suspend CEC operations (system sleep)
  resume                            Resume CEC operations (system wake)
  help                              Show this help

Options:
  --socket-path=PATH                Set path to daemon socket
  --config=/path/to/config.conf     Set path to config file

SOURCE_ID mapping:
  0   - General AV input
  1   - Audio input
  2   - HDMI 1
  3   - HDMI 2
  4   - HDMI 3
  5   - HDMI 4

DEVICE_ID typically ranges from 0-15 and maps to CEC logical addresses:
  0   - TV
  1   - Recording Device 1
  4   - Playback Device 1 (e.g., DVD/Blu-ray player)
  5   - Audio System
```

## Configuration

CEC Control uses an INI-style configuration file. The default location is:
- System-wide: `/etc/cec-control/config.conf`
- User-specific: `~/.config/cec-control/config.conf`

### Configuration Examples

```ini
[Adapter]
# Name displayed by the CEC device on the network
DeviceName = CEC Control
# Whether to automatically wake the TV when usb is powered
AutoPowerOn = true
# Whether to activate as source on the bus when starting the application
ActivateSource = true

[Daemon]
# Whether to scan for devices at startup
ScanDevicesAtStartup = true
# Whether to queue commands during suspend
QueueCommandsDuringSuspend = true

[Throttler]
# Base interval between commands (milliseconds)
BaseIntervalMs = 150
# Maximum interval between commands (milliseconds)
MaxIntervalMs = 1000
# Maximum retry attempts for failed commands
MaxRetryAttempts = 3
```

## File Locations

CEC Control follows the [XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/latest/):

### System Service Mode
- Config File: `/etc/cec-control/config.conf`
- Log File: `/var/log/cec-control/daemon.log`
- Socket Path: `/run/cec-control/socket`

### User Mode
- Config File: `~/.config/cec-control/config.conf`
- Log File: `~/.cache/cec-control/daemon.log`
- Socket Path: `$XDG_RUNTIME_DIR/cec-control/socket` (typically `/run/user/$UID/cec-control/socket`)

Environment variables can override these paths:
- `CEC_CONTROL_CONFIG`: Override the config file path
- `CEC_CONTROL_LOG`: Override the log file path
- `CEC_CONTROL_SOCKET`: Override the socket path

## Troubleshooting

### Common Issues

1. **No CEC adapter found**
   
   Ensure your HDMI-CEC adapter is properly connected and supported by libcec.
   
   ```bash
   # Check if libcec can see your adapter
   cec-client -l
   ```

2. **Permission Denied**

   When running as a normal user, check that you have permissions to access the CEC adapter:
   
   ```bash
   sudo usermod -a -G plugdev $USER  # May vary depending on your system
   ```

3. **Client Cannot Connect to Daemon**

   Verify that the daemon is running:
   
   ```bash
   # For user service
   systemctl --user status cec-daemon
   
   # For system service
   sudo systemctl status cec-daemon
   ```
   
   Check the socket path:
   
   ```bash
   # For system service, use:
   CEC_CONTROL_SOCKET=/run/cec-control/socket cec-client help
   ```

4. **Commands Not Working**

   Some TVs may have limited CEC support. Check the daemon logs for details:
   
   ```bash
   cat ~/.cache/cec-control/daemon.log
   ```

## Advanced Usage

### Integrating with Home Automation

CEC Control can be easily integrated with home automation systems:

```bash
# Example shell script to control TV with home automation
#!/bin/bash
case "$1" in
  "tv_on")
    cec-client power on 0
    ;;
  "tv_off")
    cec-client power off 0
    ;;
  "volume_up")
    cec-client volume up 5
    ;;
  "volume_down")
    cec-client volume down 5
    ;;
esac
```

### Wake/Sleep Device Lists

Configure which devices to wake or put to sleep automatically:

```ini
[Adapter]
# Comma-separated list of logical addresses (0-15) to wake on resume
WakeDevices = 0,5
# Comma-separated list of logical addresses (0-15) to power off on suspend
PowerOffDevices = 0,5
```

## Development

### Building in Debug Mode

```bash
mkdir build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
```

### Running the Daemon in Debug Mode

```bash
./build-debug/cec-daemon -v -f
```

## License

This project is licensed under the MIT License - see the LICENSE file for details.

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add some amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## Acknowledgements

- [libCEC](https://github.com/Pulse-Eight/libcec) for providing the CEC communication library
- All contributors who have helped with code, bug reports, and suggestions
