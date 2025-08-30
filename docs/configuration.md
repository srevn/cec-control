# CEC Control Configuration

CEC Control uses an INI-style configuration file for customizing its behavior. The program follows the XDG Base Directory Specification for finding configuration, cache, and runtime files.

## Specifying an Alternative Configuration File

### For the daemon:

```bash
cec-control --daemon -c /path/to/custom/config.conf
```

or 

```bash
cec-control --daemon --config /path/to/custom/config.conf
```

### For the client:

```bash
cec-control --config=/path/to/custom/config.conf power on 0
```

## Available Configuration Options

### Adapter Section

Controls CEC adapter behavior:

```ini
[Adapter]
# Name displayed by the CEC device on the network
DeviceName = CEC Control

# Whether to automatically wake the TV when usb is powered
AutoPowerOn = false

# Whether to wake the AVR automatically when the source is activated
AutoWakeAVR = false

# Whether to activate as source on the bus when starting the application
ActivateSource = false

# Whether to use audiosystem mode
SystemAudioMode = false

# Whether to put this PC in standby mode when the TV is switched off
PowerOffOnStandby = false

# Comma-separated list of logical addresses (0-15) to wake on resume
WakeDevices = 

# Comma-separated list of logical addresses (0-15) to power off on suspend
PowerOffDevices = 
```

### Daemon Section

Controls the behavior of the daemon itself:

```ini
[Daemon]
# Whether to scan for available devices at startup
ScanDevicesAtStartup = false

# Whether to queue commands during system suspend
QueueCommandsDuringSuspend = true

# Enable D-Bus power state monitoring for suspend/resume handling
# (works with WakeDevices and PowerOffDevices)
EnablePowerMonitor = true
```

### Throttler Section

Controls the command throttling parameters:

```ini
[Throttler]
# Base interval between commands in milliseconds
BaseIntervalMs = 200

# Maximum interval between commands in milliseconds
MaxIntervalMs = 1000

# Maximum number of retry attempts for failed commands
MaxRetryAttempts = 3
```

## Boolean Values

The following string values are recognized as Boolean true:
- `true`, `yes`, `1`, `on` (case insensitive)

Any other value is considered false.
