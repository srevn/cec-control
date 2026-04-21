#include "dbus_monitor.h"
#include "../common/logger.h"

#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <utility>

namespace cec_control {

namespace {

/** RAII owner for sd_bus_message*. Used on error paths so we never
 *  leak a partially-built call message before it reaches sd_bus_send. */
struct BusMessageDeleter {
    void operator()(sd_bus_message* m) const noexcept {
        if (m) sd_bus_message_unref(m);
    }
};
using BusMessagePtr = std::unique_ptr<sd_bus_message, BusMessageDeleter>;


/**
 * Convert an absolute CLOCK_MONOTONIC µs timestamp (the shape
 * sd_bus_get_timeout returns) into a relative millisecond duration.
 * UINT64_MAX → -1 sentinel for "no deadline"; values already in the
 * past map to 0; oversized deltas are clamped to INT32_MAX to stay
 * well inside timerfd's itimerspec range.
 */
std::chrono::milliseconds relativeFromAbsoluteUs(uint64_t absUs) {
    if (absUs == UINT64_MAX) {
        return std::chrono::milliseconds(-1);
    }

    struct timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return std::chrono::milliseconds(0);
    }
    const uint64_t nowUs =
        static_cast<uint64_t>(ts.tv_sec) * 1'000'000ull +
        static_cast<uint64_t>(ts.tv_nsec) / 1000ull;

    if (absUs <= nowUs) return std::chrono::milliseconds(0);

    const uint64_t deltaUs = absUs - nowUs;
    // Round up so a sub-millisecond timeout never rounds to "already fired".
    const uint64_t deltaMs = deltaUs / 1000ull + ((deltaUs % 1000ull) ? 1ull : 0ull);
    constexpr uint64_t kMaxMs = 2'147'483'647ull; // INT32_MAX
    return std::chrono::milliseconds(std::min(deltaMs, kMaxMs));
}

uint32_t pollToEventMask(int pollEvents) noexcept {
    uint32_t mask = 0;
    if (pollEvents & POLLIN)  mask |= static_cast<uint32_t>(EventPoller::Event::READ);
    if (pollEvents & POLLOUT) mask |= static_cast<uint32_t>(EventPoller::Event::WRITE);
    return mask;
}

} // namespace

DBusMonitor::DBusMonitor() = default;

DBusMonitor::~DBusMonitor() {
    detach();
    stop();
}

bool DBusMonitor::initialize() {
    LOG_INFO("Initializing sd-bus D-Bus monitor");

    int r = sd_bus_default_system(&m_bus);
    if (r < 0) {
        LOG_ERROR("Failed to connect to system D-Bus: ", busErrorToString(r));
        m_bus = nullptr;
        return false;
    }

    r = sd_bus_add_match(m_bus, &m_signalSlot,
        "type='signal',"
        "interface='org.freedesktop.login1.Manager',"
        "member='PrepareForSleep',"
        "path='/org/freedesktop/login1'",
        &DBusMonitor::onPrepareForSleep, this);

    if (r < 0) {
        LOG_ERROR("Failed to add D-Bus signal match: ", busErrorToString(r));
        sd_bus_unref(m_bus);
        m_bus = nullptr;
        return false;
    }

    if (!takeInhibitLock()) {
        LOG_WARNING("Failed to take initial inhibitor lock");
    }

    LOG_INFO("sd-bus D-Bus monitor initialized successfully");
    return true;
}

void DBusMonitor::setCallback(PowerStateCallback cb) {
    m_callback = std::move(cb);
}

bool DBusMonitor::attach(EventLoop& loop) {
    if (!m_bus) {
        LOG_ERROR("DBusMonitor::attach: not initialized");
        return false;
    }
    if (m_loop) {
        LOG_ERROR("DBusMonitor::attach: already attached");
        return false;
    }
    if (!m_timer.valid() || !m_reconnectTimer.valid()) {
        LOG_ERROR("DBusMonitor::attach: timer fd invalid");
        return false;
    }

    const int busFd = sd_bus_get_fd(m_bus);
    if (busFd < 0) {
        LOG_ERROR("sd_bus_get_fd failed: ", busErrorToString(busFd));
        return false;
    }

    m_loop = &loop;
    m_registeredBusFd = busFd;
    m_registeredMask = pollToEventMask(sd_bus_get_events(m_bus));

    const auto READ = static_cast<uint32_t>(EventPoller::Event::READ);

    if (!loop.add(busFd, m_registeredMask,
                  [this](uint32_t) { this->onBusReadable(); })) {
        m_loop = nullptr;
        m_registeredBusFd = -1;
        m_registeredMask = 0;
        return false;
    }
    if (!loop.add(m_timer.fd(), READ,
                  [this](uint32_t) { this->onTimerFire(); })) {
        loop.remove(busFd);
        m_loop = nullptr;
        m_registeredBusFd = -1;
        m_registeredMask = 0;
        return false;
    }
    if (!loop.add(m_reconnectTimer.fd(), READ,
                  [this](uint32_t) { this->onReconnectTimer(); })) {
        loop.remove(busFd);
        loop.remove(m_timer.fd());
        m_loop = nullptr;
        m_registeredBusFd = -1;
        m_registeredMask = 0;
        return false;
    }

    // Drain and arm the timer to reflect sd-bus's initial state.
    processBus();
    updateLoopRegistration();
    return true;
}

void DBusMonitor::detach() {
    if (!m_loop) return;
    if (m_registeredBusFd >= 0) {
        m_loop->remove(m_registeredBusFd);
        m_registeredBusFd = -1;
    }
    if (m_timer.valid()) {
        m_loop->remove(m_timer.fd());
    }
    if (m_reconnectTimer.valid()) {
        m_loop->remove(m_reconnectTimer.fd());
    }
    m_loop = nullptr;
}

void DBusMonitor::stop() {
    // Idempotent: a subsequent call with nothing left to tear down
    // should not re-log the stop banner. Reconnecting/Disabled both
    // leave m_bus null + m_inhibitFd <0 but still carry non-default
    // state that we reset before claiming "already stopped".
    const bool alreadyStopped =
        !m_bus && m_inhibitFd < 0 && m_state == BusState::Operational;
    if (alreadyStopped) return;

    LOG_INFO("Stopping sd-bus D-Bus monitor");

    releaseInhibitLock();
    m_timer.disarm();
    m_reconnectTimer.disarm();
    tearDownBusState();
    m_state = BusState::Operational;
    m_reconnectSchedule.reset();

    LOG_INFO("sd-bus D-Bus monitor stopped");
}

bool DBusMonitor::takeInhibitLock() {
    if (!m_bus) {
        LOG_ERROR("D-Bus connection not initialized");
        return false;
    }
    if (m_inhibitFd >= 0) {
        LOG_DEBUG("Already have inhibitor lock (fd=", m_inhibitFd, ")");
        return true;
    }
    if (m_inhibitSlot) {
        LOG_DEBUG("Inhibit request already in flight; not re-scheduling");
        return true;
    }

    // Runtime path (post-attach): asynchronous so the main loop never
    // blocks on the bus. See the class-level doc for why a sync call
    // here would strand any PrepareForSleep that arrives during the
    // wait inside sd-bus's internal queue.
    if (m_loop) {
        LOG_INFO("Scheduling asynchronous Inhibit request");
        const int r = sd_bus_call_method_async(m_bus, &m_inhibitSlot,
            "org.freedesktop.login1",
            "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager",
            "Inhibit",
            &DBusMonitor::onInhibitReply, this,
            "ssss",
            "sleep",
            "cec-control",
            "Preparing CEC adapter for sleep",
            "delay");
        if (r < 0) {
            LOG_ERROR("Failed to schedule Inhibit method call: ", busErrorToString(r));
            m_inhibitSlot = nullptr;
            return false;
        }
        // The outbox now holds the request; update the loop so POLLOUT
        // is armed on the bus fd and the next processBus() will flush.
        updateLoopRegistration();
        return true;
    }

    // Pre-attach path (initialize() only). A synchronous call is safe
    // because the signal match was just installed and no PrepareForSleep
    // could yet have been queued against it.
    LOG_INFO("Taking systemd inhibitor lock (synchronous, pre-attach)");

    sd_bus_message* rawReply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    const int r = sd_bus_call_method(m_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        &error,
        &rawReply,
        "ssss",
        "sleep",
        "cec-control",
        "Preparing CEC adapter for sleep",
        "delay");
    if (r < 0) {
        LOG_ERROR("Failed to call Inhibit method: ",
                  error.message ? error.message : busErrorToString(r));
        sd_bus_error_free(&error);
        return false;
    }
    BusMessagePtr reply(rawReply);

    int tempFd = -1;
    const int readR = sd_bus_message_read(reply.get(), "h", &tempFd);
    if (readR < 0) {
        LOG_ERROR("Failed to read inhibitor file descriptor: ", busErrorToString(readR));
        return false;
    }

    m_inhibitFd = ::dup(tempFd);
    if (m_inhibitFd < 0) {
        LOG_ERROR("Failed to duplicate inhibitor file descriptor: ", std::strerror(errno));
        return false;
    }

    LOG_INFO("Successfully took inhibitor lock (fd=", m_inhibitFd, ")");
    return true;
}

int DBusMonitor::onInhibitReply(sd_bus_message* msg, void* userdata,
                                sd_bus_error* /*ret_error*/) {
    auto* monitor = static_cast<DBusMonitor*>(userdata);
    if (!monitor) return 0;

    // sd-bus implicitly releases the slot as part of dispatching this
    // reply; drop our handle so a subsequent takeInhibitLock() doesn't
    // treat the (now-gone) request as still pending.
    monitor->m_inhibitSlot = nullptr;

    if (sd_bus_message_is_method_error(msg, nullptr)) {
        const sd_bus_error* e = sd_bus_message_get_error(msg);
        LOG_WARNING("Inhibit reply returned error: ",
                    (e && e->message) ? e->message : "unknown");
        return 0;
    }

    int fd = -1;
    const int r = sd_bus_message_read(msg, "h", &fd);
    if (r < 0) {
        LOG_ERROR("Failed to read inhibitor fd from async reply: ",
                  busErrorToString(r));
        return 0;
    }

    // If somebody released the previous lock between scheduling and
    // this reply arriving (safety timer during a stuck suspend,
    // operator action), close the stale fd before installing the new
    // one. Normal path: m_inhibitFd is already -1.
    if (monitor->m_inhibitFd >= 0) {
        ::close(monitor->m_inhibitFd);
        monitor->m_inhibitFd = -1;
    }

    monitor->m_inhibitFd = ::dup(fd);
    if (monitor->m_inhibitFd < 0) {
        LOG_ERROR("Failed to duplicate inhibitor file descriptor: ",
                  std::strerror(errno));
        return 0;
    }

    LOG_INFO("Successfully took inhibitor lock (fd=", monitor->m_inhibitFd, ")");
    return 0;
}

bool DBusMonitor::releaseInhibitLock() noexcept {
    if (m_inhibitFd < 0) {
        LOG_DEBUG("No inhibitor lock to release");
        return true;
    }
    LOG_INFO("Releasing inhibitor lock (fd=", m_inhibitFd, ")");
    ::close(m_inhibitFd);
    m_inhibitFd = -1;
    return true;
}

bool DBusMonitor::suspendSystem() {
    if (!m_bus) {
        LOG_ERROR("D-Bus connection not initialized");
        return false;
    }

    LOG_INFO("Initiating system suspend via D-Bus");

    // Floating slot (nullptr): sd-bus manages the lifetime and frees
    // it after onSuspendReply fires. The reply carries no payload we
    // need; it exists so authorization/bus errors are logged instead
    // of silently dropped.
    const int r = sd_bus_call_method_async(m_bus, nullptr,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Suspend",
        &DBusMonitor::onSuspendReply, this,
        "b",
        1);  // interactive=true
    if (r < 0) {
        LOG_ERROR("Failed to schedule Suspend method call: ", busErrorToString(r));
        return false;
    }

    // Message is queued in sd-bus's outbox; the main loop flushes it
    // on the next iteration once POLLOUT is armed via the updated mask.
    updateLoopRegistration();
    return true;
}

int DBusMonitor::onSuspendReply(sd_bus_message* msg, void* /*userdata*/,
                                sd_bus_error* /*ret_error*/) {
    if (sd_bus_message_is_method_error(msg, nullptr)) {
        const sd_bus_error* e = sd_bus_message_get_error(msg);
        LOG_WARNING("Suspend reply returned error: ",
                    (e && e->message) ? e->message : "unknown");
    } else {
        LOG_DEBUG("Suspend method call acknowledged by logind");
    }
    return 0;
}

void DBusMonitor::processBus() {
    if (m_state != BusState::Operational || !m_bus) return;

    while (true) {
        int r = sd_bus_process(m_bus, nullptr);
        if (r < 0) {
            LOG_ERROR("sd_bus_process failed: ", busErrorToString(r));
            // Disambiguate transient vs fatal: sd_bus_is_open returns
            // 1 while the peer is reachable, 0 after a clean hang-up,
            // negative on an irrecoverable connection error. Only the
            // "still open" case is worth ignoring on this pass.
            if (sd_bus_is_open(m_bus) <= 0) {
                handleBusDisconnect();
                return;
            }
            break;
        }
        if (r == 0) break;  // No more pending events in this pass.
    }
}

void DBusMonitor::updateLoopRegistration() {
    if (m_state != BusState::Operational || !m_loop || !m_bus) return;

    const int busFd = sd_bus_get_fd(m_bus);
    if (busFd < 0) {
        LOG_ERROR("sd_bus_get_fd failed: ", busErrorToString(busFd));
        handleBusDisconnect();
        return;
    }

    if (busFd != m_registeredBusFd) {
        // sd-bus is not expected to change its fd at runtime, but be
        // defensive: re-register under the new fd.
        if (m_registeredBusFd >= 0) m_loop->remove(m_registeredBusFd);
        m_registeredBusFd = busFd;
        m_registeredMask = pollToEventMask(sd_bus_get_events(m_bus));
        if (!m_loop->add(busFd, m_registeredMask,
                         [this](uint32_t) { this->onBusReadable(); })) {
            LOG_ERROR("Failed to re-register bus fd after rotation");
            m_registeredBusFd = -1;
            return;
        }
    } else {
        const uint32_t newMask = pollToEventMask(sd_bus_get_events(m_bus));
        if (newMask != m_registeredMask) {
            if (m_loop->modify(busFd, newMask)) {
                m_registeredMask = newMask;
            }
        }
    }

    uint64_t absUs = UINT64_MAX;
    const int r = sd_bus_get_timeout(m_bus, &absUs);
    if (r < 0) {
        LOG_WARNING("sd_bus_get_timeout failed: ", busErrorToString(r));
        m_timer.disarm();
        return;
    }

    const auto relMs = relativeFromAbsoluteUs(absUs);
    if (relMs.count() < 0) {
        m_timer.disarm();
    } else if (!m_timer.armOnce(relMs)) {
        // A timerfd failure here means the next purely-timeout-driven
        // bus wake-up will not fire; incoming I/O still kicks
        // processBus() via onBusReadable(). Log and carry on.
        LOG_WARNING("sd-bus timeout timer arm failed; relying on bus I/O to drive next process pass");
    }
}

void DBusMonitor::onBusReadable() {
    processBus();
    updateLoopRegistration();
}

void DBusMonitor::onTimerFire() {
    m_timer.consume();
    processBus();
    updateLoopRegistration();
}

void DBusMonitor::onReconnectTimer() {
    m_reconnectTimer.consume();
    if (m_state != BusState::Reconnecting) return;

    // attemptsSoFar was incremented by the nextDelay() call that armed
    // this timer, so it already names the attempt about to run.
    LOG_INFO("Attempting D-Bus reconnection (",
             m_reconnectSchedule.attemptsSoFar(), "/",
             m_reconnectSchedule.size(), ")");

    if (reconnectBus() && registerBusWithLoop()) {
        m_state = BusState::Operational;
        m_reconnectSchedule.reset();
        LOG_INFO("D-Bus reconnected successfully; power monitoring resumed");
        // Drain any events accumulated before we registered and resync
        // the sd-bus timer to whatever deadline the library is carrying.
        processBus();
        updateLoopRegistration();
        return;
    }

    // Clean up any half-initialised connection state before the next
    // attempt. tearDownBusState is idempotent on already-null members.
    tearDownBusState();

    const auto delay = m_reconnectSchedule.nextDelay();
    if (!delay) {
        LOG_WARNING("D-Bus reconnection abandoned after ",
                    m_reconnectSchedule.attemptsSoFar(),
                    " attempts; power monitoring disabled for this session");
        m_state = BusState::Disabled;
        return;
    }

    LOG_WARNING("D-Bus reconnect attempt failed; next try in ",
                delay->count(), "ms");
    if (!m_reconnectTimer.armOnce(*delay)) {
        LOG_ERROR("Failed to arm reconnect timer; "
                  "power monitoring disabled for this session");
        m_state = BusState::Disabled;
    }
}

void DBusMonitor::handleBusDisconnect() {
    if (m_state != BusState::Operational) return;
    LOG_WARNING("D-Bus connection lost; attempting reconnection");

    // The inhibit lock fd is tied to the dead bus: release it so logind
    // isn't left waiting on a holder that no longer exists. A fresh
    // lock will be taken as part of reconnectBus() on success.
    releaseInhibitLock();

    // Remove the bus fd from the loop before unref-ing the bus itself.
    // Removing first keeps the loop's pending batch from dispatching on
    // a handle we're about to invalidate.
    if (m_loop && m_registeredBusFd >= 0) {
        m_loop->remove(m_registeredBusFd);
        m_registeredBusFd = -1;
    }
    m_registeredMask = 0;
    m_timer.disarm();

    tearDownBusState();

    m_state = BusState::Reconnecting;
    m_reconnectSchedule.reset();
    const auto initialDelay = m_reconnectSchedule.nextDelay();
    if (!initialDelay || !m_reconnectTimer.armOnce(*initialDelay)) {
        LOG_ERROR("Failed to arm reconnect timer; "
                  "power monitoring disabled for this session");
        m_state = BusState::Disabled;
    }
}

bool DBusMonitor::reconnectBus() {
    int r = sd_bus_default_system(&m_bus);
    if (r < 0) {
        LOG_WARNING("sd_bus_default_system failed during reconnect: ",
                    busErrorToString(r));
        m_bus = nullptr;
        return false;
    }

    r = sd_bus_add_match(m_bus, &m_signalSlot,
        "type='signal',"
        "interface='org.freedesktop.login1.Manager',"
        "member='PrepareForSleep',"
        "path='/org/freedesktop/login1'",
        &DBusMonitor::onPrepareForSleep, this);
    if (r < 0) {
        LOG_WARNING("sd_bus_add_match failed during reconnect: ",
                    busErrorToString(r));
        return false;
    }

    // The inhibit lock is best-effort on reconnect: PrepareForSleep
    // delivery is what the daemon actually depends on, and it works
    // even without a delay-inhibitor. Log if it fails and carry on.
    if (!takeInhibitLock()) {
        LOG_WARNING("Reconnected to D-Bus but could not reacquire inhibit lock; "
                    "PrepareForSleep handling continues without delay guard");
    }
    return true;
}

bool DBusMonitor::registerBusWithLoop() {
    if (!m_loop || !m_bus) return false;

    const int busFd = sd_bus_get_fd(m_bus);
    if (busFd < 0) {
        LOG_ERROR("sd_bus_get_fd failed during reconnect: ",
                  busErrorToString(busFd));
        return false;
    }

    m_registeredBusFd = busFd;
    m_registeredMask = pollToEventMask(sd_bus_get_events(m_bus));
    if (!m_loop->add(busFd, m_registeredMask,
                     [this](uint32_t) { this->onBusReadable(); })) {
        LOG_ERROR("Failed to register bus fd with loop during reconnect");
        m_registeredBusFd = -1;
        m_registeredMask = 0;
        return false;
    }
    return true;
}

void DBusMonitor::tearDownBusState() noexcept {
    // Cancel any in-flight Inhibit request before dropping the bus so
    // the reply callback can't fire against a dead connection. sd-bus
    // does not invoke callbacks on slot unref; the call is simply
    // cancelled.
    if (m_inhibitSlot) {
        sd_bus_slot_unref(m_inhibitSlot);
        m_inhibitSlot = nullptr;
    }
    if (m_signalSlot) {
        sd_bus_slot_unref(m_signalSlot);
        m_signalSlot = nullptr;
    }
    if (m_bus) {
        sd_bus_unref(m_bus);
        m_bus = nullptr;
    }
}

int DBusMonitor::onPrepareForSleep(sd_bus_message* msg, void* userdata,
                                   sd_bus_error* /*ret_error*/) {
    auto* monitor = static_cast<DBusMonitor*>(userdata);
    if (!monitor) {
        LOG_ERROR("Invalid userdata in PrepareForSleep callback");
        return 0;
    }

    int sleeping = 0;
    const int r = sd_bus_message_read(msg, "b", &sleeping);
    if (r < 0) {
        LOG_ERROR("Failed to read PrepareForSleep signal parameter: ",
                  monitor->busErrorToString(r));
        return 0;
    }

    if (!monitor->m_callback) {
        LOG_WARNING("No callback registered for power state changes");
        return 0;
    }

    if (sleeping) {
        LOG_INFO("System is preparing to sleep");
        monitor->m_callback(PowerState::Suspending);
    } else {
        LOG_INFO("System is waking up");
        monitor->m_callback(PowerState::Resuming);
    }

    return 0;
}

const char* DBusMonitor::busErrorToString(int error) noexcept {
    return std::strerror(-error);
}

} // namespace cec_control
