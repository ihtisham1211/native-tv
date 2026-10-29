#include "daemon/child_process.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace nativetv {
namespace daemon {
namespace {
void closeFd(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool makePipe(int fds[2], std::string &error) {
    if (pipe(fds) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFD);
        if (flags < 0 || fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC) != 0) {
            error = std::string("fcntl(FD_CLOEXEC): ") + std::strerror(errno);
            closeFd(fds[0]);
            closeFd(fds[1]);
            return false;
        }
    }
    return true;
}

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void appendOutput(int fd, std::string &destination, size_t limit,
                  bool capture, const std::function<void(const std::string &)> &onChunk,
                  bool &overflow, bool &eof) {
    char buffer[8192];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            size_t amount = static_cast<size_t>(count);
            if (onChunk)
                onChunk(std::string(buffer, amount));
            if (capture && destination.size() < limit) {
                size_t remaining = limit - destination.size();
                size_t kept = amount < remaining ? amount : remaining;
                destination.append(buffer, kept);
                if (kept != amount)
                    overflow = true;
            } else if (capture) {
                overflow = true;
            }
            continue;
        }
        if (count == 0) {
            eof = true;
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        eof = true;
        return;
    }
}

void terminate(pid_t pid, bool &sent, bool &killed,
               std::chrono::steady_clock::time_point &killAt,
               std::chrono::milliseconds grace) {
    if (sent)
        return;
    sent = true;
    killAt = std::chrono::steady_clock::now() + grace;
    (void)kill(pid, SIGTERM);
    if (grace.count() <= 0) {
        killed = true;
        (void)kill(pid, SIGKILL);
    }
}

std::vector<char *> makePointers(const std::vector<std::string> &strings) {
    std::vector<char *> pointers;
    pointers.reserve(strings.size() + 1);
    for (const std::string &string : strings)
        pointers.push_back(const_cast<char *>(string.c_str()));
    pointers.push_back(nullptr);
    return pointers;
}
} // namespace

ChildProcessRunner::ChildProcessRunner(ChildRunnerOptions options)
    : options_(options), bornAt_(std::chrono::steady_clock::now()) {}

ChildResult ChildProcessRunner::run(const ChildOptions &options) {
    ChildResult result;
    if (options.arguments.empty() || options.arguments.front().empty()) {
        result.error = "child argv must include an executable";
        return result;
    }

    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    if (!makePipe(stdoutPipe, result.error) || !makePipe(stderrPipe, result.error)) {
        closeFd(stdoutPipe[0]); closeFd(stdoutPipe[1]);
        closeFd(stderrPipe[0]); closeFd(stderrPipe[1]);
        return result;
    }

    posix_spawn_file_actions_t actions;
    int spawnError = posix_spawn_file_actions_init(&actions);
    const bool actionsInitialized = spawnError == 0;
    if (spawnError == 0)
        spawnError = posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
    if (spawnError == 0)
        spawnError = posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
    if (spawnError == 0)
        spawnError = posix_spawn_file_actions_addclose(&actions, stdoutPipe[0]);
    if (spawnError == 0)
        spawnError = posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
    if (spawnError == 0)
        spawnError = posix_spawn_file_actions_addclose(&actions, stdoutPipe[1]);
    if (spawnError == 0)
        spawnError = posix_spawn_file_actions_addclose(&actions, stderrPipe[1]);
    if (spawnError != 0) {
        result.error = std::string("posix_spawn file actions: ") + std::strerror(spawnError);
        if (actionsInitialized)
            posix_spawn_file_actions_destroy(&actions);
        closeFd(stdoutPipe[0]); closeFd(stdoutPipe[1]);
        closeFd(stderrPipe[0]); closeFd(stderrPipe[1]);
        return result;
    }

    std::vector<std::string> arguments = options.arguments;
    std::vector<char *> argv = makePointers(arguments);
    std::vector<char *> envp = options.environment.empty()
                                   ? std::vector<char *>() : makePointers(options.environment);
    pid_t pid = -1;
    Ticket ticket(options.urgent);
    {
        std::unique_lock<std::mutex> lock(launchMutex_);
        if (ticket.urgent) {
            std::deque<Ticket *>::iterator position = launches_.begin();
            while (position != launches_.end() && (*position)->urgent)
                ++position;
            launches_.insert(position, &ticket);
        } else {
            launches_.push_back(&ticket);
        }
        launchCondition_.notify_all();

        for (;;) {
            while (!launches_.empty() && launches_.front() != &ticket)
                launchCondition_.wait(lock);
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextLaunchAt_)
                break;
            launchCondition_.wait_until(lock, nextLaunchAt_);
        }

        launches_.pop_front();
        result.startedAt = std::chrono::steady_clock::now();
        const auto gap = result.startedAt - bornAt_ < options_.startupWindow
                             ? options_.startupGap : options_.regularGap;
        nextLaunchAt_ = result.startedAt + gap;
        if (options.environment.empty()) {
            spawnError = posix_spawn(&pid, arguments[0].c_str(), &actions, nullptr,
                                     argv.data(), environ);
        } else {
            spawnError = posix_spawn(&pid, arguments[0].c_str(), &actions, nullptr,
                                     argv.data(), envp.data());
        }
    }
    launchCondition_.notify_all();
    posix_spawn_file_actions_destroy(&actions);

    closeFd(stdoutPipe[1]);
    closeFd(stderrPipe[1]);
    if (spawnError != 0) {
        result.error = std::string("posix_spawn: ") + std::strerror(spawnError);
        closeFd(stdoutPipe[0]);
        closeFd(stderrPipe[0]);
        result.endedAt = std::chrono::steady_clock::now();
        return result;
    }

    setNonBlocking(stdoutPipe[0]);
    setNonBlocking(stderrPipe[0]);
    const auto deadline = options.timeout.count() > 0
                              ? result.startedAt + options.timeout
                              : std::chrono::steady_clock::time_point::max();
    auto killAt = std::chrono::steady_clock::time_point::max();
    bool stdoutEof = false;
    bool stderrEof = false;
    bool reaped = false;
    bool terminateSent = false;
    bool killSent = false;
    int status = 0;

    while (!reaped || !stdoutEof || !stderrEof) {
        auto now = std::chrono::steady_clock::now();
        if (!terminateSent && options.cancellation &&
            options.cancellation->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            terminate(pid, terminateSent, killSent, killAt, options_.terminateGrace);
        }
        if (!terminateSent && options.timeout.count() > 0 && now >= deadline) {
            result.timedOut = true;
            terminate(pid, terminateSent, killSent, killAt, options_.terminateGrace);
        }
        if (terminateSent && !killSent && now >= killAt) {
            killSent = true;
            (void)kill(pid, SIGKILL);
        }

        pollfd fds[2];
        nfds_t count = 0;
        if (!stdoutEof) {
            fds[count].fd = stdoutPipe[0];
            fds[count].events = POLLIN | POLLHUP | POLLERR;
            fds[count].revents = 0;
            ++count;
        }
        if (!stderrEof) {
            fds[count].fd = stderrPipe[0];
            fds[count].events = POLLIN | POLLHUP | POLLERR;
            fds[count].revents = 0;
            ++count;
        }

        int waitMs = 50;
        if (terminateSent && !killSent) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(killAt - now).count();
            if (remaining >= 0 && remaining < waitMs)
                waitMs = static_cast<int>(remaining);
        } else if (options.timeout.count() > 0) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            if (remaining >= 0 && remaining < waitMs)
                waitMs = static_cast<int>(remaining);
        }
        int pollResult = poll(fds, count, waitMs);
        if (pollResult < 0 && errno != EINTR) {
            result.error = std::string("poll child output: ") + std::strerror(errno);
            if (!terminateSent)
                terminate(pid, terminateSent, killSent, killAt, options_.terminateGrace);
        }

        size_t perStreamLimit = options.maximumOutputBytes;
        appendOutput(stdoutPipe[0], result.standardOutput, perStreamLimit,
                     options.captureOutput, options.onStandardOutput,
                     result.outputLimitExceeded, stdoutEof);
        appendOutput(stderrPipe[0], result.standardError, perStreamLimit,
                     options.captureOutput, options.onStandardError,
                     result.outputLimitExceeded, stderrEof);
        if (result.outputLimitExceeded && !terminateSent)
            terminate(pid, terminateSent, killSent, killAt, options_.terminateGrace);

        if (!reaped) {
            pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid) {
                reaped = true;
            } else if (waited < 0 && errno != EINTR) {
                result.error = std::string("waitpid: ") + std::strerror(errno);
                reaped = true;
            }
        }
    }

    closeFd(stdoutPipe[0]);
    closeFd(stderrPipe[0]);
    if (reaped && WIFEXITED(status))
        result.exitCode = WEXITSTATUS(status);
    else if (reaped && WIFSIGNALED(status))
        result.signal = WTERMSIG(status);
    result.endedAt = std::chrono::steady_clock::now();
    return result;
}
} // namespace daemon
} // namespace nativetv
