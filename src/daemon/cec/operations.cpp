#include "operations.h"

#include "../../common/logger.h"
#include "../command_throttler.h"
#include "adapter_interface.h"

#include <chrono>
#include <ios>
#include <thread>

#include <libcec/cec.h>

namespace cec_control::ops {

namespace {

// HDMI source IDs as documented in --help: 2..5 map to HDMI 1..4. The CEC
// physical address byte layout is 0xN000 where N is the HDMI port number.
constexpr uint8_t kFirstHdmiSource = 2;
constexpr uint8_t kLastHdmiSource  = 5;

uint16_t hdmiPhysicalAddress(uint8_t source) noexcept {
    return static_cast<uint16_t>((source - kFirstHdmiSource + 1) << 12);
}

CEC::cec_user_control_code hdmiNumberKey(uint8_t source) noexcept {
    switch (source) {
        case 2: return CEC::CEC_USER_CONTROL_CODE_NUMBER1;
        case 3: return CEC::CEC_USER_CONTROL_CODE_NUMBER2;
        case 4: return CEC::CEC_USER_CONTROL_CODE_NUMBER3;
        case 5: return CEC::CEC_USER_CONTROL_CODE_NUMBER4;
    }
    return CEC::CEC_USER_CONTROL_CODE_UNKNOWN;
}

} // namespace

bool powerOnDevice(ICecAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Powering on device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter, logicalAddress]() {
        const auto addr = static_cast<CEC::cec_logical_address>(logicalAddress);
        if (!adapter.isDeviceActive(addr)) {
            LOG_WARNING("Device ", static_cast<int>(logicalAddress), " is not active");
        }
        return adapter.powerOnDevice(addr);
    });
}

bool powerOffDevice(ICecAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Powering off device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter, logicalAddress]() {
        const auto addr = static_cast<CEC::cec_logical_address>(logicalAddress);
        if (!adapter.isDeviceActive(addr)) {
            LOG_WARNING("Device ", static_cast<int>(logicalAddress), " is not active");
        }
        return adapter.standbyDevice(addr);
    });
}

bool setVolume(ICecAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress, bool up) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Setting volume ", up ? "up" : "down",
             " on device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter, up]() {
        return up ? adapter.volumeUp() : adapter.volumeDown();
    });
}

bool setMute(ICecAdapter& adapter, CommandThrottler& throttler, uint8_t logicalAddress, bool mute) {
    if (!adapter.isConnected()) return false;
    LOG_INFO(mute ? "Muting" : "Unmuting", " device ", static_cast<int>(logicalAddress));
    return throttler.executeWithThrottle([&adapter]() {
        return adapter.toggleMute();
    });
}

bool setSource(ICecAdapter& adapter, CommandThrottler& throttler, uint8_t source) {
    if (!adapter.isConnected()) return false;
    LOG_INFO("Selecting input source ", static_cast<int>(source), " on TV");

    return throttler.executeWithThrottle([&adapter, source]() {
        // Sources 0 and 1 are TV-internal inputs without a CEC physical
        // address; SetStreamPath cannot reach them, so go straight to a
        // function-key keypress.
        auto sendKey = [&adapter](CEC::cec_user_control_code key) {
            if (!adapter.sendKeypress(CEC::CECDEVICE_TV, key, false)) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            (void)adapter.sendKeypress(CEC::CECDEVICE_TV, CEC::CEC_USER_CONTROL_CODE_UNKNOWN, true);
            return true;
        };

        if (source == 0) return sendKey(CEC::CEC_USER_CONTROL_CODE_SELECT_AV_INPUT_FUNCTION);
        if (source == 1) return sendKey(CEC::CEC_USER_CONTROL_CODE_SELECT_AUDIO_INPUT_FUNCTION);
        if (source < kFirstHdmiSource || source > kLastHdmiSource) {
            LOG_WARNING("Invalid source value: ", source);
            return false;
        }

        // HDMI input: SetStreamPath is the canonical mechanism; fall back to
        // INPUT_SELECT + number keypress sequence if the TV refuses.
        const uint16_t physicalAddress = hdmiPhysicalAddress(source);
        LOG_INFO("Setting stream path to physical address: 0x",
                 std::hex, physicalAddress);

        if (adapter.setStreamPath(physicalAddress)) {
            return true;
        }

        LOG_INFO("SetStreamPath failed, trying with key presses");
        if (!adapter.sendKeypress(CEC::CECDEVICE_TV,
                                  CEC::CEC_USER_CONTROL_CODE_INPUT_SELECT, false)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (!adapter.sendKeypress(CEC::CECDEVICE_TV, hdmiNumberKey(source), false)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)adapter.sendKeypress(CEC::CECDEVICE_TV, CEC::CEC_USER_CONTROL_CODE_UNKNOWN, true);
        return true;
    });
}

void logDeviceSnapshot(ICecAdapter& adapter) {
    try {
        const CEC::cec_logical_addresses addresses = adapter.getActiveDevices();

        int activeCount = 0;
        for (int i = 0; i < 16; ++i) {
            if (addresses[i]) ++activeCount;
        }
        LOG_INFO("Found ", activeCount, " active CEC device(s)");

        LOG_INFO("Scanning for CEC devices power status...");
        for (int i = 0; i < 15; ++i) {
            const auto cecAddress = static_cast<CEC::cec_logical_address>(i);
            const char* status = "unknown";
            switch (adapter.getDevicePowerStatus(cecAddress)) {
                case CEC::CEC_POWER_STATUS_ON:                          status = "ON"; break;
                case CEC::CEC_POWER_STATUS_STANDBY:                     status = "STANDBY"; break;
                case CEC::CEC_POWER_STATUS_IN_TRANSITION_STANDBY_TO_ON: status = "TURNING ON"; break;
                case CEC::CEC_POWER_STATUS_IN_TRANSITION_ON_TO_STANDBY: status = "TURNING OFF"; break;
                default: break;
            }
            LOG_INFO("Device ", i, ": Power status = ", status);
        }

        const CEC::cec_logical_address active = adapter.getActiveSource();
        if (active != CEC::CECDEVICE_UNKNOWN) {
            LOG_INFO("Active source: Device ", static_cast<int>(active));
        } else {
            LOG_INFO("No active source detected");
        }

        for (int i = 0; i < 16; ++i) {
            if (!addresses[i]) continue;
            const auto addr = static_cast<CEC::cec_logical_address>(i);
            LOG_INFO("Device ", i, ": ", adapter.getDeviceOSDName(addr),
                     " (", adapter.isDeviceActive(addr) ? "active" : "inactive", ")");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Exception during device scanning: ", e.what());
    }
}

} // namespace cec_control::ops
