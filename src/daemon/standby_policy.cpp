#include "standby_policy.h"

#include <utility>

#include "../common/logger.h"

namespace cec_control {

StandbyPolicy::StandbyPolicy(bool initialEnabled,
                             std::function<void()> onSuspendRequested)
    : m_enabled(initialEnabled),
      m_suspendCallback(std::move(onSuspendRequested)) {}

Message StandbyPolicy::apply(const Message& command) {
    if (command.data.empty()) {
        return Message(MessageType::RESP_ERROR);
    }
    m_enabled = command.data[0] > 0;
    LOG_INFO("Auto-standby ", m_enabled ? "enabled" : "disabled");
    return Message(MessageType::RESP_SUCCESS);
}

void StandbyPolicy::observe(const ICecAdapter::Observation& obs) {
    if (obs.kind != ICecAdapter::Observation::Kind::TvStandby) return;

    if (!m_enabled) {
        LOG_DEBUG("TV standby observed; auto-standby disabled - ignoring");
        return;
    }
    if (!m_suspendCallback) {
        LOG_WARNING("TV standby observed but no suspend callback wired");
        return;
    }
    LOG_INFO("TV standby observed with auto-standby enabled; "
             "initiating system suspend");
    m_suspendCallback();
}

bool StandbyPolicy::isEnabled() const noexcept {
    return m_enabled;
}

} // namespace cec_control
