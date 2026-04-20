#include "systemd_env.h"

#include <cstdlib>
#include <initializer_list>

namespace cec_control {
namespace SystemdEnv {

bool isUnderSystemd() noexcept {
    static const bool cached = [] {
        for (const char* name : {"NOTIFY_SOCKET", "INVOCATION_ID", "SYSTEMD_EXEC_PID"}) {
            if (const char* value = std::getenv(name); value && *value) {
                return true;
            }
        }
        return false;
    }();
    return cached;
}

} // namespace SystemdEnv
} // namespace cec_control
