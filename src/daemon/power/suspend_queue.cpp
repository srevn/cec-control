#include "suspend_queue.h"

#include <utility>

namespace cec_control {

void SuspendQueue::enterSuspended() noexcept {
    m_suspended.store(true, std::memory_order_release);
}

void SuspendQueue::exitSuspended() noexcept {
    m_suspended.store(false, std::memory_order_release);
}

bool SuspendQueue::isSuspended() const noexcept {
    return m_suspended.load(std::memory_order_acquire);
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
