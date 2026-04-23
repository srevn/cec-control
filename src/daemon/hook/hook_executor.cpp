#include "hook_executor.h"

#include "../../common/logger.h"

#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <utility>
#include <vector>

namespace cec_control {

namespace {

/**
 * RAII wrapper around @c posix_spawnattr_t so an early return path
 * cannot leak the structure. Plain @c posix_spawn_file_actions_t gets
 * the same treatment — both are trivially destructible from libc's
 * standpoint but require an explicit destroy call on some platforms.
 */
class SpawnAttr {
public:
    SpawnAttr() {
        m_ok = ::posix_spawnattr_init(&m_attr) == 0;
    }
    ~SpawnAttr() {
        if (m_ok) ::posix_spawnattr_destroy(&m_attr);
    }
    SpawnAttr(const SpawnAttr&)            = delete;
    SpawnAttr& operator=(const SpawnAttr&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_ok; }
    posix_spawnattr_t* get() noexcept { return &m_attr; }

private:
    posix_spawnattr_t m_attr{};
    bool              m_ok{false};
};

class SpawnFileActions {
public:
    SpawnFileActions() {
        m_ok = ::posix_spawn_file_actions_init(&m_actions) == 0;
    }
    ~SpawnFileActions() {
        if (m_ok) ::posix_spawn_file_actions_destroy(&m_actions);
    }
    SpawnFileActions(const SpawnFileActions&)            = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_ok; }
    posix_spawn_file_actions_t* get() noexcept { return &m_actions; }

private:
    posix_spawn_file_actions_t m_actions{};
    bool                       m_ok{false};
};

/**
 * Issue one @c posix_spawn call for @p job. Logs and returns on error;
 * on success the child is running and the daemon will see the
 * eventual SIGCHLD via its signalfd.
 */
void spawnOne(HookExecutor::Job& job) {
    SpawnAttr attr;
    SpawnFileActions actions;
    if (!attr.valid() || !actions.valid()) {
        LOG_WARNING("Hook spawn setup failed for ", job.path,
                    ": posix_spawnattr/file_actions init failed");
        return;
    }

    // Undo the daemon's inherited SIG_BLOCK mask so the child sees a
    // normal signal environment. Without this, shell scripts spawned
    // from the daemon would inherit the blocked set and behave oddly
    // under @c trap.
    sigset_t emptyMask;
    sigemptyset(&emptyMask);
    if (::posix_spawnattr_setsigmask(attr.get(), &emptyMask) != 0 ||
        ::posix_spawnattr_setflags(attr.get(), POSIX_SPAWN_SETSIGMASK) != 0) {
        LOG_WARNING("Hook spawn setup failed for ", job.path,
                    ": posix_spawnattr_setsigmask failed");
        return;
    }

    // Hook scripts must not read from the daemon's stdin — under
    // systemd stdin is already closed, but belt-and-braces for the
    // foreground / dev case where a stray @c read would otherwise
    // hang forever on a terminal.
    if (::posix_spawn_file_actions_addopen(
            actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0) != 0) {
        LOG_WARNING("Hook spawn setup failed for ", job.path,
                    ": posix_spawn_file_actions_addopen(/dev/null) failed");
        return;
    }
    // Stdout and stderr deliberately inherit: under systemd journald
    // captures them under the daemon's unit; under a foreground run
    // they reach the operator's terminal. Either is correct.

    // posix_spawn takes char*[] (not const); std::string::data()
    // returns a mutable char* in C++17 and posix_spawn does not
    // modify its argv/envp. The job owns the backing strings until
    // this function returns, which is after posix_spawn has copied
    // the pointers into the child.
    char*               argv0 = job.path.data();
    std::vector<char*>  argv{argv0, nullptr};

    std::vector<char*> envp;
    envp.reserve(job.env.size() + 1);
    for (auto& entry : job.env) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, job.path.c_str(),
                                  actions.get(), attr.get(),
                                  argv.data(), envp.data());
    if (rc != 0) {
        LOG_WARNING("Hook spawn failed for ", job.path, ": ",
                    std::strerror(rc));
        return;
    }
    LOG_DEBUG("Hook spawned pid=", pid, " path=", job.path);
}

} // namespace

HookExecutor::~HookExecutor() {
    stop();
}

void HookExecutor::start() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_started) return;
    // Spawn before latching m_started: if std::thread's constructor
    // throws (resource exhaustion), the object stays in its unstarted
    // state and a later retry / destructor walks a consistent path.
    m_thread  = std::thread(&HookExecutor::run, this);
    m_started = true;
}

void HookExecutor::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopRequested) return;
        m_stopRequested = true;
    }
    m_cv.notify_one();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void HookExecutor::submit(Job job) {
    if (job.path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopRequested) return;
        m_jobs.push(std::move(job));
    }
    m_cv.notify_one();
}

void HookExecutor::run() {
    // Identifiable in `top -H` and `gdb thread apply all bt`. Kernel
    // truncates silently to 15 bytes.
    ::pthread_setname_np(::pthread_self(), "cec-hook-exec");

    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] {
                return m_stopRequested || !m_jobs.empty();
            });
            if (m_stopRequested) {
                // Matches AdapterWorker::run: jobs queued but not yet
                // spawned are dropped on stop. The main thread is
                // already quitting; spawning now would only produce
                // zombies that init reaps.
                std::queue<Job> dropped;
                dropped.swap(m_jobs);
                break;
            }
            job = std::move(m_jobs.front());
            m_jobs.pop();
        }

        try {
            spawnOne(job);
        } catch (const std::exception& e) {
            LOG_ERROR("HookExecutor spawn threw: ", e.what());
        } catch (...) {
            LOG_ERROR("HookExecutor spawn threw non-std exception");
        }
    }
}

void hook::reapChildren() noexcept {
    for (;;) {
        int status = 0;
        const pid_t pid = ::waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            // 0  = children running but none have exited yet
            // -1 = ECHILD (no children) or a transient error — either
            //      way, the queue is drained for this SIGCHLD.
            break;
        }
        if (WIFEXITED(status)) {
            LOG_DEBUG("Hook child pid=", pid,
                      " exited status=", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            LOG_DEBUG("Hook child pid=", pid,
                      " killed by signal=", WTERMSIG(status));
        }
    }
}

} // namespace cec_control
