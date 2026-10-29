#pragma once
#include <algorithm>
#include <chrono>

namespace nativetv {
// The initial /api/caps read is the app's readiness probe. Keep retries short
// so the existing failure response still reaches the UI promptly.
class ReadinessRetry {
  public:
    using Milliseconds = std::chrono::milliseconds;

    static unsigned maxRetries() {
        return 4;
    }
    static Milliseconds budget() {
        return Milliseconds(2500);
    }
    static Milliseconds delay(unsigned retry) {
        static const int delays[] = {100, 250, 500, 800};
        return retry < maxRetries() ? Milliseconds(delays[retry]) : Milliseconds(0);
    }
    static bool retryable(bool transportFailure, long status) {
        return transportFailure || status == 502 || status == 503 || status == 504;
    }
    static bool shouldRetry(unsigned retries, bool retryableFailure, Milliseconds elapsed) {
        return retries < maxRetries() && retryableFailure && elapsed < budget();
    }
    static long attemptTimeoutMs(Milliseconds elapsed) {
        return std::max<long>(0, std::min<long>(1000, (budget() - elapsed).count()));
    }
};
} // namespace nativetv
