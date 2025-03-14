# CEC Control Configuration

CEC Control uses an INI-style configuration file for customizing its behavior. By default, 
the configuration file is located at `/etc/cec-control.conf`.

## Specifying an Alternative Configuration File

### For the daemon:

```bash
cec-daemon -c /path/to/custom/config.conf
```

or 

```bash
cec-daemon --config /path/to/custom/config.conf
```

### For the client:

```bash
cec-client --config=/path/to/custom/config.conf power on 0
```

## Configuration File Format

The configuration file uses an INI-style format with sections and key-value pairs:

```ini
[SectionName]
Key1 = Value1
Key2 = Value2
```

## Available Configuration Options

### Adapter Section

Controls how the CEC adapter behaves when interacting with devices:

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

# Specific device addresses to wake up (comma-separated list of addresses, 0-15)
WakeDevices = 

# Specific device addresses to power off (comma-separated list of addresses, 0-15)
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

Boolean options can be specified using any of these values:

* `true`, `yes`, `on`, `1` for TRUE
* `false`, `no`, `off`, `0` for FALSE

Boolean values are case-insensitive.

## Examples

### Minimal Configuration

```ini
[Adapter]
DeviceName = Media Center
SystemAudioMode = true

[Daemon]
ScanDevicesAtStartup = true
```

### Full Configuration

```ini
[Adapter]
DeviceName = Home Theater PC
AutoPowerOn = true
AutoWakeAVR = true
ActivateSource = true
SystemAudioMode = true
PowerOffOnStandby = false
WakeDevices = 1,5
PowerOffDevices = 1,5,3

[Daemon]
ScanDevicesAtStartup = true
QueueCommandsDuringSuspend = true

[Throttler]
BaseIntervalMs = 150
MaxIntervalMs = 800
MaxRetryAttempts = 4
```
