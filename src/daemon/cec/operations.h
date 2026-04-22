#pragma once

#include <cstdint>

namespace cec_control {

class ICecAdapter;
class CommandThrottler;

/**
 * @namespace cec_control::ops
 * @brief Wire-command bodies: the concrete CEC actions a wire request
 *        ultimately drives.
 *
 * Every operation is pure in the sense that it holds no state of its own;
 * it reads the adapter's current connected hint, runs its action through
 * the @c CommandThrottler, and returns whether the throttled attempt
 * succeeded. No caller-side locking is required around a call — the
 * adapter serialises libcec access internally and the throttler is
 * thread-safe via atomics.
 *
 * These helpers deliberately duplicate the adapter's own
 * @c isConnected() guard: the dispatch path already gated the command,
 * but each operation short-circuits again to avoid paying for a throttle
 * slot when the adapter has dropped between the two checks.
 */
namespace ops {

/**
 * HDMI source IDs accepted on the wire. Source values in
 * [@c kFirstHdmiSource, @c kLastHdmiSource] map to HDMI 1..4 via the CEC
 * physical-address layout @c 0xN000 (where @c N is the port number).
 * Kept public so help rendering can guard against drift without
 * redeclaring the range.
 */
inline constexpr uint8_t kFirstHdmiSource = 2;
inline constexpr uint8_t kLastHdmiSource  = 5;

/** Wake @p logicalAddress via @c ICecAdapter::powerOnDevice, throttled. */
[[nodiscard]] bool powerOnDevice(ICecAdapter& adapter,
                                 CommandThrottler& throttler,
                                 uint8_t logicalAddress);

/** Send standby to @p logicalAddress via @c ICecAdapter::standbyDevice, throttled. */
[[nodiscard]] bool powerOffDevice(ICecAdapter& adapter,
                                  CommandThrottler& throttler,
                                  uint8_t logicalAddress);

/** Throttled volume step. @p up selects VolumeUp vs. VolumeDown. */
[[nodiscard]] bool setVolume(ICecAdapter& adapter,
                             CommandThrottler& throttler,
                             uint8_t logicalAddress,
                             bool up);

/** Throttled mute toggle. The @p mute argument is informational (CEC
 *  exposes only a toggle) and drives the log line. */
[[nodiscard]] bool setMute(ICecAdapter& adapter,
                           CommandThrottler& throttler,
                           uint8_t logicalAddress,
                           bool mute);

/**
 * Drive the TV's input selector. Source IDs:
 *  - @c 0 / @c 1 map to TV-internal AV / Audio inputs (keypress only — no
 *    physical address for SetStreamPath to target),
 *  - @c 2..5 map to HDMI 1..4 via SetStreamPath, with an INPUT_SELECT +
 *    number-key keypress fallback if SetStreamPath is refused.
 *
 * The action always targets @c CEC::CECDEVICE_TV; a logical-address
 * parameter would be misleading and is deliberately absent.
 */
[[nodiscard]] bool setSource(ICecAdapter& adapter,
                             CommandThrottler& throttler,
                             uint8_t source);

/**
 * Throttled CEC user-control press-and-release targeting
 * @p logicalAddress. Emits one @c SendKeypress followed by
 * @c SendKeyRelease after a short inter-step delay; release is
 * best-effort (CEC 1.4b §13.13 guarantees receivers auto-release
 * within ~500 ms, so a failed release does not leave the target
 * stuck — matching the @c setSource precedent).
 *
 * @p code is the raw CEC user-control byte; callers are expected to
 * validate against @ref kKeyCodes at the wire gate (see
 * @c handleKey in @c command_dispatch.cpp).
 */
[[nodiscard]] bool sendKey(ICecAdapter& adapter,
                           CommandThrottler& throttler,
                           uint8_t logicalAddress,
                           uint8_t code);

/**
 * Log a one-shot snapshot of active CEC devices and their power status.
 * Used during daemon start-up to capture the bus at boot. Not a wire
 * command — kept alongside the command bodies because it also operates
 * solely through @c ICecAdapter.
 */
void logDeviceSnapshot(ICecAdapter& adapter);

} // namespace ops
} // namespace cec_control
