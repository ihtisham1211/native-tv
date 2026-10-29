#pragma once

#include <curl/curl.h>
#include <mutex>

namespace nativetv {
namespace service {

inline bool ensureCurlGlobal() {
    static std::once_flag initialized;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return result == CURLE_OK;
}

} // namespace service
} // namespace nativetv
