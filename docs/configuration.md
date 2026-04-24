# CEC Control Configuration

CEC Control uses an INI-style configuration file for customizing its behavior. The program follows the XDG Base Directory Specification for finding configuration, cache, and runtime files.

## Specifying an Alternative Configuration File

The configuration file is read by the daemon only. Specify it with `-c` or
`--config`:

```bash
cec-control daemon -c /path/to/custom/config.conf
```

or

```bash
cec-control daemon --config /path/to/custom/config.conf
```

The client reads no configuration file. To talk to a daemon listening on a
non-default socket, pass `--socket-path=PATH` after the client command:

```bash
cec-control power on 0 --socket-path=/run/cec-control/socket
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

### Hooks Section

Map CEC bus events to external scripts. Each entry is the absolute path
to an executable that the daemon runs, fire-and-forget, when the
corresponding event is observed on the bus. Empty or missing value =
hook disabled for that event.

```ini
[Hooks]
# Run when another device announces itself as the active source.
InputSwitch = /usr/local/bin/cec-input-switch.sh

# Run when the TV reports that it is going to standby.
TVStandby = /usr/local/bin/cec-tv-off.sh

# Run when the TV reports that it has powered on (after standby).
TVWake =
```

#### Events

| Event | Trigger |
|---|---|
| `InputSwitch` | The active source on the CEC bus changed to a different physical address — e.g. the TV's active input changed, or an AVR routed HDMI to a new device. |
| `TVStandby`   | The TV reported it is going to standby. |
| `TVWake`      | The TV reported it has powered on, after having been in standby. |

#### Environment

Scripts are invoked with a sanitised environment. The parent process's
environment is **not** propagated — only the following five keys, if
set in the daemon's own environment: `PATH`, `HOME`, `LANG`, `LC_ALL`,
`USER`. On top of those, the daemon injects:

| Variable | Present for | Value |
|---|---|---|
| `CEC_EVENT` | every event | `InputSwitch` \| `TVStandby` \| `TVWake` |
| `CEC_EVENT_TS` | every event | ISO-8601 UTC timestamp, e.g. `2026-04-23T12:34:56Z` |
| `CEC_DAEMON_PID` | every event | decimal PID of the daemon, for log correlation |
| `CEC_SOURCE_PHYSICAL` | `InputSwitch` | dotted nibble form, e.g. `2.0.0.0` |
| `CEC_SOURCE_PHYSICAL_RAW` | `InputSwitch` | raw 16-bit value, e.g. `0x2000` |
| `CEC_SOURCE_PREVIOUS_PHYSICAL` | `InputSwitch` | dotted form of the previous active source, or empty string on the first event |
| `CEC_TV_POWER` | `TVStandby`, `TVWake` | `standby` \| `on` |
| `CEC_TV_POWER_PREVIOUS` | `TVStandby`, `TVWake` | `on` \| `standby` \| empty string (no prior state known) |

`*_PREVIOUS` env vars are **empty strings** the first time the
corresponding event fires; scripts should check with `[ -z "$VAR" ]`.

#### Known limitation: `TVWake`

`TVWake` depends on the TV emitting an **unsolicited**
`REPORT_POWER_STATUS` message when it powers on. Many TVs do this;
some emit `REPORT_POWER_STATUS` only in response to a
`GIVE_DEVICE_POWER_STATUS` query. On those TVs, `TVWake` will not
fire through passive observation alone.

## Boolean Values

The following string values are recognized as Boolean true:
- `true`, `yes`, `1`, `on` (case insensitive)

Any other value is considered false.
