#include "key_codes.h"

#include <algorithm>

namespace cec_control {

// Codes mirror libcec's cec_user_control_code enumerators. Keeping the
// raw bytes here (rather than the libcec enum values directly) keeps
// this TU free of libcec includes, so the common/ layer stays independent
// of the CEC backend. The corresponding static_asserts in
// daemon/cec/operations.cpp pin each row to its libcec enumerator.
const std::array<KeySpec, 4> kKeyCodes = {{
    {"blue",   0x71},  // CEC_USER_CONTROL_CODE_F1_BLUE
    {"red",    0x72},  // CEC_USER_CONTROL_CODE_F2_RED
    {"green",  0x73},  // CEC_USER_CONTROL_CODE_F3_GREEN
    {"yellow", 0x74},  // CEC_USER_CONTROL_CODE_F4_YELLOW
}};

const KeySpec* findKeyByName(std::string_view name) noexcept {
    const auto it = std::find_if(kKeyCodes.begin(), kKeyCodes.end(),
        [name](const KeySpec& s) { return s.name == name; });
    return it == kKeyCodes.end() ? nullptr : &*it;
}

const KeySpec* findKeyByCode(uint8_t code) noexcept {
    const auto it = std::find_if(kKeyCodes.begin(), kKeyCodes.end(),
        [code](const KeySpec& s) { return s.code == code; });
    return it == kKeyCodes.end() ? nullptr : &*it;
}

std::string formatKeyNamesList(std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < kKeyCodes.size(); ++i) {
        if (i != 0) out.append(separator.data(), separator.size());
        out.append(kKeyCodes[i].name.data(), kKeyCodes[i].name.size());
    }
    return out;
}

} // namespace cec_control
