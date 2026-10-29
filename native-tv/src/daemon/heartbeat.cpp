#include "daemon/heartbeat.hpp"
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace nativetv {
namespace daemon {
namespace {
bool writeTimestamp(const std::string &path) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    bool ok = fchmod(fd, 0600) == 0;
    char stamp[32];
    int length = std::snprintf(stamp, sizeof(stamp), "%lld\n",
                               static_cast<long long>(std::time(nullptr)));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(stamp))
        ok = false;
    size_t offset = 0;
    while (ok && offset < static_cast<size_t>(length)) {
        ssize_t written = write(fd, stamp + offset, static_cast<size_t>(length) - offset);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (close(fd) != 0)
        ok = false;
    return ok;
}
} // namespace

Heartbeat::Heartbeat(EventLoop &loop, std::string path, EventLoop::Milliseconds interval)
    : loop_(loop), state_(std::make_shared<State>(std::move(path))), interval_(interval) {
    if (state_->path.empty() || interval_.count() <= 0)
        throw std::invalid_argument("heartbeat path and interval must be valid");
}

Heartbeat::~Heartbeat() {
    stop();
}

bool Heartbeat::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_->active.load())
        return false;
    state_->failure = false;
    state_->active = true;
    std::shared_ptr<State> state = state_;
    EventLoop *loop = &loop_;
    EventLoop::Task beat = [state, loop]() {
        if (!state->active.load())
            return;
        if (!loop->isLoopThread() || !writeTimestamp(state->path)) {
            state->failure = true;
            state->active = false;
            loop->stop();
            return;
        }
        ++state->writeCount;
    };
    timer_ = loop_.scheduleEvery(interval_, beat);
    if (!timer_ || !loop_.post(beat)) {
        if (timer_)
            loop_.cancelTimer(timer_);
        timer_ = 0;
        state_->active = false;
        return false;
    }
    return true;
}

void Heartbeat::stop() {
    EventLoop::TimerId timer = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_->active = false;
        timer = timer_;
        timer_ = 0;
    }
    if (timer)
        loop_.cancelTimer(timer);
}

bool Heartbeat::running() const {
    return state_->active.load();
}

bool Heartbeat::failed() const {
    return state_->failure.load();
}

unsigned long Heartbeat::writes() const {
    return state_->writeCount.load();
}
} // namespace daemon
} // namespace nativetv
