#include "suspend_queue.h"

#include <utility>

namespace cec_control {

void SuspendQueue::enterSuspended() noexcept {
    m_suspended = true;
}

void SuspendQueue::exitSuspended() noexcept {
    m_suspended = false;
}

bool SuspendQueue::isSuspended() const noexcept {
    return m_suspended;
}

bool SuspendQueue::tryPush(const Message& command) {
    if (!isSuspended()) return false;
    m_queued.push_back(command);
    return true;
}

std::vector<Message> SuspendQueue::drain() {
    return std::exchange(m_queued, std::vector<Message>{});
}

} // namespace cec_control
