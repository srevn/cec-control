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

# Whether to automatically power on devices
AutoPowerOn = false

# Whether to automatically wake the AVR
AutoWakeAVR = false

# Whether to activate as a source
ActivateSource = false

# Whether to use system audio mode
SystemAudioMode = false

# Whether to power off connected devices on system standby
PowerOffOnStandby = false
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

[Daemon]
ScanDevicesAtStartup = true
QueueCommandsDuringSuspend = true

[Throttler]
BaseIntervalMs = 150
MaxIntervalMs = 800
MaxRetryAttempts = 4
```
