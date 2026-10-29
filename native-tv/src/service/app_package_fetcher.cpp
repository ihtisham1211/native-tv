#include "service/app_package_fetcher.hpp"
#include "service/curl_global.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <vector>

namespace nativetv {
namespace service {
namespace {

const size_t kMaximumUrlCodeUnits = 2048;
const uint64_t kMaximumDownloadBytes = 512ULL * 1024 * 1024;

bool startsWithScheme(const std::string &url, const char *scheme) {
    const size_t size = std::char_traits<char>::length(scheme);
    if (url.size() < size) return false;
    for (size_t index = 0; index < size; ++index) {
        if (std::tolower(static_cast<unsigned char>(url[index])) != scheme[index])
            return false;
    }
    return true;
}

void nextCodePoint(const std::string &value, size_t &index, unsigned long &codePoint) {
    const unsigned char lead = static_cast<unsigned char>(value[index]);
    size_t bytes = 1;
    codePoint = lead;
    if (lead >= 0xc2 && lead <= 0xdf && index + 1 < value.size()) {
        const unsigned char second = static_cast<unsigned char>(value[index + 1]);
        if ((second & 0xc0) == 0x80) {
            bytes = 2;
            codePoint = ((lead & 0x1f) << 6) | (second & 0x3f);
        }
    } else if (lead >= 0xe0 && lead <= 0xef && index + 2 < value.size()) {
        const unsigned char second = static_cast<unsigned char>(value[index + 1]);
        const unsigned char third = static_cast<unsigned char>(value[index + 2]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
            !(lead == 0xe0 && second < 0xa0) &&
            !(lead == 0xed && second >= 0xa0)) {
            bytes = 3;
            codePoint = ((lead & 0x0f) << 12) | ((second & 0x3f) << 6) |
                        (third & 0x3f);
        }
    } else if (lead >= 0xf0 && lead <= 0xf4 && index + 3 < value.size()) {
        const unsigned char second = static_cast<unsigned char>(value[index + 1]);
        const unsigned char third = static_cast<unsigned char>(value[index + 2]);
        const unsigned char fourth = static_cast<unsigned char>(value[index + 3]);
        if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80 &&
            (fourth & 0xc0) == 0x80 && !(lead == 0xf0 && second < 0x90) &&
            !(lead == 0xf4 && second >= 0x90)) {
            bytes = 4;
            codePoint = ((lead & 0x07) << 18) | ((second & 0x3f) << 12) |
                        ((third & 0x3f) << 6) | (fourth & 0x3f);
        }
    }
    index += bytes;
}

struct DownloadContext {
    CURL *curl = nullptr;
    uint64_t maximumBytes = 0;
    uint64_t receivedBytes = 0;
    AppPackageDownloadSink sink;
    AppPackageDownloadCancelled cancellationRequested;
    bool tooLarge = false;
    bool interrupted = false;
    bool cancelled = false;
    std::string errorBody;
};

int checkCancellation(void *opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    DownloadContext *context = static_cast<DownloadContext *>(opaque);
    if (!context->cancellationRequested || !context->cancellationRequested()) return 0;
    context->cancelled = true;
    return 1;
}

size_t receive(char *bytes, size_t size, size_t count, void *opaque) {
    DownloadContext *context = static_cast<DownloadContext *>(opaque);
    if (!size || count > std::numeric_limits<size_t>::max() / size) return 0;
    const size_t length = size * count;
    long status = 0;
    (void)curl_easy_getinfo(context->curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        if (context->errorBody.size() < 4096) {
            const size_t copy = std::min(length, 4096 - context->errorBody.size());
            context->errorBody.append(bytes, copy);
        }
        return length;
    }
    if (length > context->maximumBytes - context->receivedBytes) {
        context->tooLarge = true;
        return 0;
    }
    if (!context->sink || !context->sink(bytes, length)) {
        context->interrupted = true;
        return 0;
    }
    context->receivedBytes += length;
    return length;
}

} // namespace

bool AppPackageFetcher::validUrl(const std::string &url, std::string &error) {
    error.clear();
    if (!startsWithScheme(url, "http://") && !startsWithScheme(url, "https://")) {
        error = "the address must start with http:// or https://";
        return false;
    }
    size_t codeUnits = 0;
    bool hasControlCharacter = false;
    for (size_t index = 0; index < url.size();) {
        unsigned long codePoint = 0;
        nextCodePoint(url, index, codePoint);
        codeUnits += codePoint > 0xffff ? 2 : 1;
        hasControlCharacter = hasControlCharacter || codePoint <= 0x20 ||
                              (codePoint >= 0x7f && codePoint <= 0x9f);
    }
    if (codeUnits > kMaximumUrlCodeUnits) {
        error = "the address is longer than 2048 characters";
        return false;
    }
    if (hasControlCharacter) {
        error = "the address contains spaces or control characters";
        return false;
    }
    return true;
}

bool AppPackageFetcher::download(const std::string &url, const std::string &userAgent,
                                const std::string &ipPreference, uint64_t maximumBytes,
                                const AppPackageDownloadSink &sink, long &httpStatus,
                                AppPackageDownloadFailure &failure,
                                const AppPackageDownloadCancelled &cancelled) {
    httpStatus = 0;
    failure = AppPackageDownloadFailure();
    if (!validUrl(url, failure.message)) return false;
    if (!maximumBytes || maximumBytes > kMaximumDownloadBytes) {
        failure.message = "invalid package download size limit";
        return false;
    }
    if (!sink) {
        failure.message = "package download sink is unavailable";
        return false;
    }
    if (!ensureCurlGlobal()) {
        failure.message = "could not initialize libcurl";
        return false;
    }

    std::vector<long> addressFamilies;
    if (ipPreference == "4") {
        addressFamilies.push_back(CURL_IPRESOLVE_V4);
        addressFamilies.push_back(CURL_IPRESOLVE_V6);
    } else if (ipPreference == "6") {
        addressFamilies.push_back(CURL_IPRESOLVE_V6);
        addressFamilies.push_back(CURL_IPRESOLVE_V4);
    } else {
        addressFamilies.push_back(CURL_IPRESOLVE_WHATEVER);
        addressFamilies.push_back(CURL_IPRESOLVE_V6);
        addressFamilies.push_back(CURL_IPRESOLVE_V4);
    }

    CURLcode result = CURLE_FAILED_INIT;
    DownloadContext context;
    for (size_t attempt = 0; attempt < addressFamilies.size(); ++attempt) {
        CURL *curl = curl_easy_init();
        if (!curl) {
            failure.message = "could not create a libcurl request";
            return false;
        }
        context = DownloadContext();
        context.curl = curl;
        context.maximumBytes = maximumBytes;
        context.sink = sink;
        context.cancellationRequested = cancelled;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 30000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 30L * 60 * 1000);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
        curl_easy_setopt(curl, CURLOPT_IPRESOLVE, addressFamilies[attempt]);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#pragma GCC diagnostic pop
        curl_easy_setopt(curl, CURLOPT_PROXY, "");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, checkCancellation);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);

        result = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        curl_easy_cleanup(curl);

        const bool canRetry = (result == CURLE_COULDNT_RESOLVE_HOST ||
                               result == CURLE_COULDNT_CONNECT ||
                               result == CURLE_OPERATION_TIMEDOUT) &&
                              context.receivedBytes == 0 && httpStatus == 0 &&
                              !context.tooLarge && !context.interrupted &&
                              !context.cancelled &&
                              attempt + 1 < addressFamilies.size();
        if (!canRetry) break;
    }

    if (context.cancelled) {
        failure.message = "cancelled";
        return false;
    }
    if (context.tooLarge) {
        failure.message = "download exceeds the allowed size";
        return false;
    }
    if (context.interrupted) {
        failure.message = "package download stream was interrupted";
        return false;
    }
    if (result != CURLE_OK) {
        failure.message = curl_easy_strerror(result);
        if (!context.errorBody.empty()) failure.message += ": " + context.errorBody;
        failure.offline = result == CURLE_COULDNT_RESOLVE_HOST ||
                          result == CURLE_COULDNT_CONNECT;
        failure.timedOut = result == CURLE_OPERATION_TIMEDOUT;
        return false;
    }
    if (httpStatus != 200) {
        failure.message = "remote server returned HTTP " + std::to_string(httpStatus);
        return false;
    }
    return true;
}

} // namespace service
} // namespace nativetv
