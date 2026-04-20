#include "dbus_monitor.h"
#include "../common/logger.h"

#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace cec_control {

namespace {

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

/**
 * Delay between reconnect attempts after a bus disconnect. Tuned so
 * that a quick dbus-daemon or logind restart (sub-second downtime) is
 * caught by the first retry, while a genuine outage gets exponentially
 * longer waits. After the final entry expires the monitor transitions
 * to Disabled; the daemon continues serving CEC work without further
 * retry attempts until restarted.
 */
constexpr std::array<std::chrono::milliseconds, 4> kReconnectSchedule = {
    std::chrono::milliseconds(2'000),
    std::chrono::milliseconds(10'000),
    std::chrono::milliseconds(30'000),
    std::chrono::milliseconds(60'000),
};

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
        LOG_WARNING("Failed to take initial inhibitor lock - sleep delays may not work properly");
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
    m_reconnectAttempts = 0;

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

    LOG_INFO("Taking systemd inhibitor lock");

    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(m_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        &error,
        &reply,
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

    int tempFd = -1;
    r = sd_bus_message_read(reply, "h", &tempFd);
    if (r < 0) {
        LOG_ERROR("Failed to read inhibitor file descriptor: ", busErrorToString(r));
        sd_bus_message_unref(reply);
        return false;
    }

    m_inhibitFd = ::dup(tempFd);
    sd_bus_message_unref(reply);

    if (m_inhibitFd < 0) {
        LOG_ERROR("Failed to duplicate inhibitor file descriptor: ", std::strerror(errno));
        return false;
    }

    LOG_INFO("Successfully took inhibitor lock (fd=", m_inhibitFd, ")");
    return true;
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

    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(m_bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Suspend",
        &error,
        &reply,
        "b",
        1);  // interactive=true

    if (r < 0) {
        LOG_ERROR("Failed to call Suspend method: ",
                  error.message ? error.message : busErrorToString(r));
        sd_bus_error_free(&error);
        return false;
    }

    if (reply) sd_bus_message_unref(reply);

    LOG_INFO("System suspend initiated successfully via D-Bus");
    return true;
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
    } else {
        m_timer.armOnce(relMs);
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

    LOG_INFO("Attempting D-Bus reconnection (",
             m_reconnectAttempts + 1, "/",
             kReconnectSchedule.size(), ")");

    if (reconnectBus() && registerBusWithLoop()) {
        m_state = BusState::Operational;
        m_reconnectAttempts = 0;
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
    ++m_reconnectAttempts;

    if (m_reconnectAttempts >= kReconnectSchedule.size()) {
        LOG_WARNING("D-Bus reconnection abandoned after ",
                    m_reconnectAttempts,
                    " attempts; power monitoring disabled for this session");
        m_state = BusState::Disabled;
        return;
    }

    const auto delay = kReconnectSchedule[m_reconnectAttempts];
    LOG_WARNING("D-Bus reconnect attempt failed; next try in ",
                delay.count(), "ms");
    if (!m_reconnectTimer.armOnce(delay)) {
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
    m_reconnectAttempts = 0;

    if (!m_reconnectTimer.armOnce(kReconnectSchedule[0])) {
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
