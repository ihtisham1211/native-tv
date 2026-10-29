#include "daemon/hostname_cache.hpp"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>

namespace nativetv {
namespace daemon {

struct HostnameCache::State {
    std::mutex mutex;
    std::string address;
    std::string name;
    long long at = 0;
    std::string asking;  // the address being looked up
};

std::string hostnameFromPtr(std::string answer) {
    while (!answer.empty() && answer.back() == '.') answer.pop_back();
    return answer;
}

std::string reverseLookup(const std::string &address) {
    sockaddr_in socket = {};
    socket.sin_family = AF_INET;
    if (inet_pton(AF_INET, address.c_str(), &socket.sin_addr) != 1) return std::string();
    char host[256] = {};
    if (getnameinfo(reinterpret_cast<sockaddr *>(&socket), sizeof(socket), host, sizeof(host),
                    nullptr, 0, NI_NAMEREQD) != 0)
        return std::string();
    return hostnameFromPtr(host);
}

HostnameCache::HostnameCache(Resolve resolve, Clock nowMs, long long timeoutMs, long long ttlMs)
    : state_(std::make_shared<State>()),
      resolve_(resolve ? std::move(resolve) : Resolve(reverseLookup)),
      nowMs_(nowMs ? std::move(nowMs) : Clock([]() {
          return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count());
      })),
      timeoutMs_(timeoutMs), ttlMs_(ttlMs) {}

std::string HostnameCache::name(const std::string &address) {
    if (address.empty()) return std::string();
    const std::shared_ptr<State> state = state_;
    std::lock_guard<std::mutex> lock(state->mutex);
    const bool known = state->address == address;
    const std::string cached = known ? state->name : std::string();
    if ((!known || nowMs_() - state->at >= ttlMs_) && state->asking != address) {
        state->asking = address;
        // Whichever of the answer and the timeout comes first is the result; the
        // other finds it published and does nothing.
        const std::shared_ptr<std::atomic<bool>> published = std::make_shared<std::atomic<bool>>(false);
        const Clock now = nowMs_;
        const auto publish = [state, address, now, published](const std::string &found) {
            if (published->exchange(true)) return;
            std::lock_guard<std::mutex> guard(state->mutex);
            if (state->asking != address) return;  // the TV has left that address
            state->asking.clear();
            state->address = address;
            state->name = found;
            state->at = now();
        };
        const Resolve resolve = resolve_;
        std::thread([resolve, address, publish]() { publish(resolve(address)); }).detach();
        const long long timeout = timeoutMs_;
        std::thread([timeout, publish]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
            publish(std::string());
        }).detach();
    }
    return cached;
}

} // namespace daemon
} // namespace nativetv
