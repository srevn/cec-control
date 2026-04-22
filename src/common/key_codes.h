#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cec_control {

/**
 * Human-readable name paired with its CEC user-control-code byte.
 *
 * @c code is the raw wire-level byte the daemon emits via libcec's
 * @c SendKeypress; it mirrors the libcec @c cec_user_control_code
 * enumerator of the same meaning. Static_asserts in
 * @c daemon/cec/operations.cpp pin each row to its libcec enumerator
 * so drift between this table and libcec breaks the build rather than
 * silently corrupting the CEC message at runtime.
 */
struct KeySpec {
    std::string_view name;
    uint8_t          code;
};

/**
 * Authoritative name <-> code mapping for the client @c key command.
 *
 * Adding a new key is a single new row here, consumed by:
 *  - the client-side parser (name -> code), in @c command_registry.cpp,
 *  - the daemon-side allowlist + log rendering (code -> name), in
 *    @c command_dispatch.cpp and @c operations.cpp,
 *  - the @c --help output (enumerates valid names), in
 *    @c help_printer.cpp.
 *
 * The size literal in the declaration tracks the definition in
 * @c key_codes.cpp; a mismatch is a compile-time error. When extending
 * the set, add a matching @c static_assert alongside the existing ones
 * in @c operations.cpp.
 */
extern const std::array<KeySpec, 4> kKeyCodes;

/** Linear lookup by canonical name. Returns nullptr if no match. */
[[nodiscard]] const KeySpec* findKeyByName(std::string_view name) noexcept;

/** Linear lookup by wire-level code byte. Returns nullptr if no match. */
[[nodiscard]] const KeySpec* findKeyByCode(uint8_t code) noexcept;

/**
 * Render the names in @c kKeyCodes, in iteration order, joined by
 * @p separator. Used by the parser's error text and the client help
 * output; keeping the rendering in one place prevents the two call
 * sites from drifting out of sync with the table.
 */
[[nodiscard]] std::string formatKeyNamesList(std::string_view separator);

} // namespace cec_control
