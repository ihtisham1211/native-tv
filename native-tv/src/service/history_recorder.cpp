#include "service/history_recorder.hpp"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace nativetv {
namespace service {
namespace {
const int64_t kHourMs = 60 * 60 * 1000LL;
const int64_t kClockFloorMs = 1767225600000LL; // 2026-01-01T00:00:00Z
const size_t kMaxMonthBytes = 2 * 1024 * 1024;
const unsigned kKeepMonths = 13;

int64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double monotonicNow() {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string hourKey(int64_t milliseconds) {
    const time_t seconds = static_cast<time_t>(milliseconds / 1000);
    struct tm utc;
    if (!gmtime_r(&seconds, &utc))
        return std::string();
    char value[14] = {};
    return ::strftime(value, sizeof(value), "%Y-%m-%dT%H", &utc) ? value : std::string();
}

std::string monthKey(const std::string &hour) {
    return hour.size() >= 7 ? hour.substr(0, 7) : std::string();
}

bool monthFilename(const char *name, std::string &month) {
    if (!name || std::strlen(name) != 12 || std::strcmp(name + 7, ".json") != 0)
        return false;
    for (unsigned i = 0; i < 7; ++i) {
        if (i == 4) {
            if (name[i] != '-') return false;
        } else if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    const int number = (name[5] - '0') * 10 + (name[6] - '0');
    if (number < 1 || number > 12)
        return false;
    month.assign(name, 7);
    return true;
}

std::string oldestKept(int64_t milliseconds) {
    const time_t seconds = static_cast<time_t>(milliseconds / 1000);
    struct tm utc;
    if (!gmtime_r(&seconds, &utc))
        return std::string();
    int year = utc.tm_year + 1900;
    int month = utc.tm_mon + 1 - static_cast<int>(kKeepMonths - 1);
    while (month <= 0) {
        month += 12;
        --year;
    }
    std::ostringstream formatted;
    formatted.width(4);
    formatted.fill('0');
    formatted << year << '-';
    formatted.width(2);
    formatted.fill('0');
    formatted << month;
    return formatted.str();
}

std::string directoryOf(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    return slash == 0 ? "/" : path.substr(0, slash);
}

bool ensureDirectory(const std::string &path, std::string *error) {
    if (path.empty() || path == "." || path == "/") return true;
    if (mkdir(path.c_str(), 0700) == 0) return true;
    if (errno == EEXIST) {
        struct stat info;
        if (stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) return true;
        if (error) *error = path + " exists but is not a directory";
        return false;
    }
    if (errno != ENOENT) {
        if (error) *error = "create directory " + path + ": " + std::strerror(errno);
        return false;
    }
    const std::string parent = directoryOf(path);
    return parent != path && ensureDirectory(parent, error) && ensureDirectory(path, error);
}

bool atomicWrite(const std::string &path, const std::string &contents, std::string *error) {
    if (!ensureDirectory(directoryOf(path), error)) return false;
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = mkstemp(name.data());
    if (fd < 0) {
        if (error) *error = "create temporary history file: " + std::string(std::strerror(errno));
        return false;
    }
    const std::string temporary(name.data());
    bool ok = fchmod(fd, 0600) == 0;
    if (!ok && error)
        *error = "set history file permissions: " + std::string(std::strerror(errno));
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ok = false;
            if (error) *error = "write history file: " + std::string(std::strerror(errno));
            break;
        }
        offset += static_cast<size_t>(count);
    }
    if (ok && fsync(fd) != 0) {
        ok = false;
        if (error) *error = "sync history file: " + std::string(std::strerror(errno));
    }
    if (close(fd) != 0 && ok) {
        ok = false;
        if (error) *error = "close history file: " + std::string(std::strerror(errno));
    }
    if (!ok) {
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        if (error) *error = "replace history file: " + std::string(std::strerror(errno));
        unlink(temporary.c_str());
        return false;
    }
    const int directory = open(directoryOf(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0 || fsync(directory) != 0) {
        if (error) *error = "sync history directory: " + std::string(std::strerror(errno));
        if (directory >= 0) close(directory);
        return false;
    }
    close(directory);
    return true;
}

std::string normalizedPowerState(const Json &value) {
    std::string key = value.string();
    key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char character) {
                  return std::isspace(character) || character == '_' || character == '-';
              }), key.end());
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return key;
}

} // namespace

HistoryRecorder::HistoryRecorder(HistoryRecorderOptions options)
    : options_(std::move(options)), enabled_(options_.enabled), running_(options_.enabled) {
    if (!options_.wallNowMs) options_.wallNowMs = wallNow;
    if (!options_.monotonicNowMs) options_.monotonicNowMs = monotonicNow;
    if (enabled_) {
        std::lock_guard<std::mutex> lock(mutex_);
        loadAllLocked();
        pruneLocked();
    }
}

void HistoryRecorder::start(const Json &snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) startLocked(snapshot);
    else state_ = snapshot;
}

void HistoryRecorder::startLocked(const Json &snapshot) {
    if (!loaded_) loadAllLocked();
    pruneLocked();
    enabled_ = true;
    running_ = true;
    state_ = snapshot;
    const Json app = snapshot["application"];
    const std::string appKey = app["app"].string();
    const std::string appId = app["app_id"].string();
    if (!appKey.empty() && !appId.empty()) appIds_[appKey] = appId;
    if (!appKey.empty() && app["display_title"].isString() &&
        app["display_title"].string() != appKey)
        titles_[appKey] = app["display_title"].string();
    const std::string key = keyForLocked(snapshot);
    currentKey_ = key;
    currentStartMonoMs_ = key.empty() ? 0 : options_.monotonicNowMs();
}

void HistoryRecorder::observe(const state::Event &event, const Json &snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = snapshot;
    if (!running_) return;

    const Json app = snapshot["application"];
    const std::string appKey = app["app"].string();
    const std::string appId = app["app_id"].string();
    if (!appKey.empty() && !appId.empty()) appIds_[appKey] = appId;
    if (!appKey.empty() && app["display_title"].isString() &&
        app["display_title"].string() != appKey)
        titles_[appKey] = app["display_title"].string();

    if ((event.group == "power" && (event.key == "state" || event.key == "screenOn" ||
                                      event.key == "systemOn")) ||
        (event.group == "application" && event.key == "app")) {
        const std::string key = keyForLocked(snapshot);
        if (key != currentKey_) {
            switchToLocked(key);
            if (key.empty() || key == "screen_off") {
                std::string ignored;
                (void)flushLocked(&ignored);
            }
        }
    }
}

bool HistoryRecorder::setEnabled(bool enabled, std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error) error->clear();
    if (enabled == enabled_) return true;
    if (enabled) {
        startLocked(state_);
        return true;
    }
    switchToLocked(std::string());
    if (!flushLocked(error)) {
        currentKey_ = keyForLocked(state_);
        currentStartMonoMs_ = currentKey_.empty() ? 0 : options_.monotonicNowMs();
        return false;
    }
    enabled_ = false;
    running_ = false;
    return true;
}

bool HistoryRecorder::clear(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error) error->clear();
    const std::string keepCurrent = running_ ? currentKey_ : std::string();
    months_.clear();
    dirty_.clear();
    pending_.clear();
    loaded_ = true;
    DIR *directory = opendir(options_.directory.c_str());
    if (directory) {
        struct dirent *entry;
        while ((entry = readdir(directory)) != nullptr) {
            std::string month;
            const std::string name(entry->d_name);
            if (monthFilename(entry->d_name, month) ||
                (name.size() > 8 && name.compare(0, 7, "20") == 0 &&
                 name.find(".json.tmp") != std::string::npos))
                (void)unlink((options_.directory + "/" + name).c_str());
        }
        closedir(directory);
    } else if (errno != ENOENT) {
        if (error) *error = "open history directory: " + std::string(std::strerror(errno));
        return false;
    }
    currentKey_ = keepCurrent;
    currentStartMonoMs_ = keepCurrent.empty() ? 0 : options_.monotonicNowMs();
    return true;
}

bool HistoryRecorder::flush(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushLocked(error);
}

void HistoryRecorder::stop(std::string *error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;
    switchToLocked(std::string());
    (void)flushLocked(error);
    running_ = false;
    enabled_ = false;
}

Json HistoryRecorder::overlay(int64_t fromMs, int64_t toMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    return overlayLocked(fromMs, toMs);
}

Json HistoryRecorder::totals(int64_t fromMs, int64_t toMs) {
    Json result = Json::object();
    if (fromMs > toMs)
        return result;
    const std::string from = hourKey(fromMs);
    const std::string to = hourKey(toMs);
    if (from.empty() || to.empty() || from > to)
        return result;

    std::lock_guard<std::mutex> lock(mutex_);
    checkpointLocked();
    for (const auto &entry : months_) {
        const Json hours = entry.second["hours"];
        for (const std::string &hour : hours.keys()) {
            if (hour < from || hour > to)
                continue;
            const Json values = hours[hour];
            for (const std::string &key : values.keys()) {
                const Json seconds = values[key];
                if (!seconds.isNumber() || !std::isfinite(seconds.number()) ||
                    seconds.number() <= 0)
                    continue;
                result.set(key, Json::numeric(result[key].number() + seconds.number()));
            }
        }
    }
    return result;
}

Json HistoryRecorder::summary(int64_t wallMs, int utcOffsetMinutes) {
    // Date.UTC(2026, 0, 1): a clock before it has not been set.
    static const int64_t kClockFloorMs = 1767225600000LL;
    static const int64_t kDayMs = 86400000LL;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return Json();
    }
    if (wallMs < kClockFloorMs) return Json();
    const int64_t offsetMs = static_cast<int64_t>(utcOffsetMinutes) * 60000;
    const int64_t local = wallMs + offsetMs;
    const int64_t dayStart = (local >= 0 ? local / kDayMs : -((-local + kDayMs - 1) / kDayMs)) * kDayMs - offsetMs;
    const Json today = totals(dayStart, wallMs);
    const Json week = totals(dayStart - 6 * kDayMs, wallMs);
    // Screen off and screen saver time is the TV on with nothing being watched.
    const auto idle = [](const std::string &key) { return key == "screen_off" || key == "screen_saver"; };
    const auto watched = [&idle](const Json &byKey) {
        double seconds = 0;
        for (const std::string &key : byKey.keys())
            if (!idle(key)) seconds += byKey[key].number();
        return seconds;
    };
    std::string top;
    for (const std::string &key : today.keys())
        if (!idle(key) && (top.empty() || today[key].number() > today[top].number())) top = key;
    // Rounded to hundredths of an hour, half up as Math.round does.
    const auto hours = [](double seconds) { return std::floor(seconds / 36 + 0.5) / 100; };
    Json out = Json::object();
    out.set("today_h", Json::numeric(hours(watched(today))));
    out.set("week_h", Json::numeric(hours(watched(week))));
    std::string title;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = titles_.find(top);
        title = found != titles_.end() && !found->second.empty() ? found->second : top;
    }
    out.set("top_today", top.empty() ? Json() : Json::text(title));
    return out;
}

Json HistoryRecorder::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    bool kept = !dirty_.empty();
    DIR *directory = opendir(options_.directory.c_str());
    if (directory) {
        struct dirent *entry;
        while (!kept && (entry = readdir(directory)) != nullptr) {
            std::string month;
            kept = monthFilename(entry->d_name, month);
        }
        closedir(directory);
    }
    Json result = Json::object();
    result.set("enabled", Json::flag(enabled_));
    result.set("running", Json::flag(running_));
    result.set("kept", Json::flag(kept));
    return result;
}

Json HistoryRecorder::overlayLocked(int64_t fromMs, int64_t toMs) {
    checkpointLocked();
    const std::string from = hourKey(fromMs);
    const std::string to = hourKey(toMs);
    Json hours = Json::object();
    Json used = Json::object();
    Json ids = Json::object();
    Json titles = Json::object();
    if (!from.empty() && !to.empty() && from <= to) {
        for (const std::string &monthName : dirty_) {
            const Json month = months_[monthName];
            const Json monthHours = month["hours"];
            for (const std::string &hour : monthHours.keys()) {
                if (hour < from || hour > to) continue;
                hours.set(hour, monthHours[hour]);
                for (const std::string &key : monthHours[hour].keys())
                    used.set(key, Json::flag(true));
            }
        }
        for (const std::string &key : used.keys()) {
            const std::string id = appIds_[key];
            const std::string title = titles_[key];
            if (!id.empty()) ids.set(key, Json::text(id));
            if (!title.empty()) titles.set(key, Json::text(title));
        }
    }
    Json result = Json::object();
    result.set("enabled", Json::flag(enabled_));
    result.set("running", Json::flag(running_));
    result.set("readPersisted", Json::flag(loaded_));
    if (running_ && !currentKey_.empty()) result.set("current", Json::text(currentKey_));
    else result.set("current", Json());
    result.set("clockSet", Json::flag(options_.wallNowMs() >= kClockFloorMs));
    result.set("hours", hours);
    result.set("titles", titles);
    result.set("ids", ids);
    return result;
}

std::string HistoryRecorder::keyForLocked(const Json &snapshot) const {
    const Json power = snapshot["power"];
    if (!power["systemOn"].isBoolean() || !power["systemOn"].boolean())
        return std::string();
    // The panel state can update before the raw power label does; Node's
    // history key uses this explicit field to stop counting watch time.
    if (!power["screenOn"].isBoolean() || !power["screenOn"].boolean())
        return "screen_off";
    const std::string state = normalizedPowerState(power["state"]);
    if (state == "screensaver") return "screen_saver";
    return snapshot["application"]["app"].string();
}

void HistoryRecorder::switchToLocked(const std::string &key) {
    const double now = options_.monotonicNowMs();
    if (!currentKey_.empty() && now > currentStartMonoMs_)
        pending_.push_back(PendingSegment{currentKey_, currentStartMonoMs_, now});
    currentKey_ = key;
    currentStartMonoMs_ = key.empty() ? 0 : now;
    placePendingLocked();
}

void HistoryRecorder::checkpointLocked() {
    if (!running_) return placePendingLocked();
    if (!currentKey_.empty()) switchToLocked(currentKey_);
    else placePendingLocked();
}

void HistoryRecorder::placePendingLocked() {
    const int64_t wall = options_.wallNowMs();
    if (pending_.empty() || wall < kClockFloorMs) return;
    const double offset = static_cast<double>(wall) - options_.monotonicNowMs();
    for (const PendingSegment &segment : pending_) {
        const int64_t start = static_cast<int64_t>(std::llround(segment.startMonoMs + offset));
        const int64_t end = static_cast<int64_t>(std::llround(segment.endMonoMs + offset));
        addTimeLocked(segment.key, start, end);
    }
    pending_.clear();
}

void HistoryRecorder::addTimeLocked(const std::string &key, int64_t startWallMs,
                                    int64_t endWallMs) {
    int64_t time = startWallMs;
    while (time < endWallMs) {
        const int64_t hourEnd = (time / kHourMs + 1) * kHourMs;
        const int64_t end = std::min(endWallMs, hourEnd);
        const std::string hour = hourKey(time);
        const std::string monthName = monthKey(hour);
        if (hour.empty() || monthName.empty()) break;
        Json month = monthLocked(monthName);
        Json hours = month["hours"];
        Json values = hours[hour];
        if (!values.isObject()) values = Json::object();
        const Json existing = values[key];
        const double seconds = std::round(((existing.isNumber() ? existing.number() : 0.0) +
                                           static_cast<double>(end - time) / 1000.0) * 10.0) /
                               10.0;
        if (seconds > 0) {
            values.set(key, Json::numeric(seconds));
            hours.set(hour, values);
            month.set("hours", hours);
            Json monthTitles = month["titles"];
            const auto title = titles_.find(key);
            if (title != titles_.end()) monthTitles.set(key, Json::text(title->second));
            month.set("titles", monthTitles);
            Json monthIds = month["ids"];
            const auto id = appIds_.find(key);
            if (id != appIds_.end()) monthIds.set(key, Json::text(id->second));
            month.set("ids", monthIds);
            months_[monthName] = month;
            dirty_.insert(monthName);
        }
        time = end;
    }
}

void HistoryRecorder::loadAllLocked() {
    loaded_ = true;
    DIR *directory = opendir(options_.directory.c_str());
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        std::string monthName;
        if (!monthFilename(entry->d_name, monthName)) continue;
        std::ifstream input((options_.directory + "/" + entry->d_name).c_str(),
                            std::ios::in | std::ios::binary);
        if (!input) continue;
        input.seekg(0, std::ios::end);
        const std::streamoff length = input.tellg();
        if (length < 0 || static_cast<size_t>(length) > kMaxMonthBytes) continue;
        input.seekg(0, std::ios::beg);
        std::string contents(static_cast<size_t>(length), '\0');
        if (length > 0) input.read(&contents[0], length);
        if (!input && length > 0) continue;
        const Json document = Json::parse(contents);
        if (!document.isObject() || !document["hours"].isObject()) continue;
        Json month = Json::object();
        month.set("v", Json::numeric(1));
        month.set("hours", document["hours"]);
        month.set("titles", document["titles"].isObject() ? document["titles"] : Json::object());
        month.set("ids", document["ids"].isObject() ? document["ids"] : Json::object());
        for (const std::string &key : month["titles"].keys())
            if (month["titles"][key].isString()) titles_[key] = month["titles"][key].string();
        for (const std::string &key : month["ids"].keys())
            if (month["ids"][key].isString()) appIds_[key] = month["ids"][key].string();
        months_[monthName] = month;
    }
    closedir(directory);
}

void HistoryRecorder::pruneLocked() {
    const int64_t wall = options_.wallNowMs();
    if (wall < kClockFloorMs) return;
    const std::string oldest = oldestKept(wall);
    if (oldest.empty()) return;
    for (auto month = months_.begin(); month != months_.end();) {
        if (month->first < oldest) {
            dirty_.erase(month->first);
            month = months_.erase(month);
        } else {
            ++month;
        }
    }
    DIR *directory = opendir(options_.directory.c_str());
    if (!directory) return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != nullptr) {
        std::string month;
        if (monthFilename(entry->d_name, month) && month < oldest)
            (void)unlink((options_.directory + "/" + entry->d_name).c_str());
    }
    closedir(directory);
}

bool HistoryRecorder::flushLocked(std::string *error) {
    if (error) error->clear();
    checkpointLocked();
    if (dirty_.empty()) return true;
    pruneLocked();
    for (auto month = dirty_.begin(); month != dirty_.end();) {
        const auto document = months_.find(*month);
        if (document == months_.end()) {
            month = dirty_.erase(month);
            continue;
        }
        const std::string path = options_.directory + "/" + *month + ".json";
        const std::string contents = document->second.dump();
        if (contents.size() > kMaxMonthBytes) {
            if (error) *error = "history month exceeds the write limit";
            return false;
        }
        if (!atomicWrite(path, contents, error)) return false;
        month = dirty_.erase(month);
    }
    return true;
}

Json HistoryRecorder::monthLocked(const std::string &monthName) {
    auto found = months_.find(monthName);
    if (found != months_.end()) return found->second;
    Json month = Json::object();
    month.set("v", Json::numeric(1));
    month.set("hours", Json::object());
    month.set("titles", Json::object());
    month.set("ids", Json::object());
    months_[monthName] = month;
    return month;
}

} // namespace service
} // namespace nativetv
