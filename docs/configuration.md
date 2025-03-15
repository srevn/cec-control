# CEC Control Configuration

CEC Control uses an INI-style configuration file for customizing its behavior. The program follows the XDG Base Directory Specification for finding configuration, cache, and runtime files.

## File Locations

CEC Control follows the [XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/latest/) exclusively. Locations are:

- **Configuration file**: 
  - `$XDG_CONFIG_HOME/cec-control/config.conf` (typically `~/.config/cec-control/config.conf`)

- **Log file**: 
  - `$XDG_CACHE_HOME/cec-control/daemon.log` (typically `~/.cache/cec-control/daemon.log`)

- **Socket file**: 
  - `$XDG_RUNTIME_DIR/cec-control/socket` (typically `/run/user/[UID]/cec-control/socket`)

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

Controls CEC adapter behavior:

```ini
[Adapter]
# Name that appears on CEC bus
DeviceName = CEC Controller

# Enable auto power on behavior 
AutoPowerOn = false

# Auto wake for audio receiver
AutoWakeAVR = true

# The device should be activated as source when powering on
ActivateSource = false

# Use system audio mode
SystemAudioMode = true

# Power off devices when going to standby
PowerOffOnStandby = true

# Comma-separated list of logical addresses (0-15) to wake on resume
WakeDevices = 0,5

# Comma-separated list of logical addresses (0-15) to power off on suspend
PowerOffDevices = 0,5
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

The following string values are recognized as Boolean true:
- `true`, `yes`, `1`, `on` (case insensitive)

Any other value is considered false.

## Examples

### Minimal Configuration

```ini
[Adapter]
DeviceName = CEC Controller
AutoPowerOn = true
ActivateSource = true

[Throttler]
BaseIntervalMs = 150
```
