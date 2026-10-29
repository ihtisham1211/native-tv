#pragma once

#include "core/json.hpp"
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace service {

using AppCatalogFetcher = std::function<bool(const std::string &, const std::string &,
                                             Json &, std::string &)>;

class AppCatalog {
  public:
    explicit AppCatalog(std::string version = "", AppCatalogFetcher fetcher = {});

    Json get(const Json &configuredRepos, const Json &installedReply, bool force,
             int64_t nowMs);
    Json findPackage(const Json &configuredRepos, const Json &installedReply,
                     const std::string &id, const std::string &repoUrl, int64_t nowMs);
    Json checkRepository(const std::string &url) const;
    void invalidate();

    static const std::string &defaultRepository();
    static Json customRepositories(const Json &configuredRepos);
    static std::string normalizeRepositoryCandidate(const std::string &input);
    static std::string repositoryInputError(const std::string &url);

    static bool fetchJson(const std::string &url, const std::string &userAgent,
                          Json &document, std::string &error);
    static bool validPackageId(const std::string &id);
    static Json parsePackage(const Json &item, const std::string &pageUrl,
                             const std::string &repoUrl);
    static std::string resolveUrl(const std::string &base, const std::string &relative);

  private:
    struct Repository {
        std::string url;
        Json apps;
        bool hasApps = false;
        std::string error;
    };

    Json fetchRepository(const std::string &url, std::string &error) const;
    Json response(const Json &installedReply) const;
    void refresh(const std::vector<std::string> &urls, int64_t nowMs);

    std::string version_;
    AppCatalogFetcher fetcher_;
    mutable std::mutex mutex_;
    std::vector<std::string> cachedUrls_;
    std::vector<Repository> repositories_;
    bool hasCache_ = false;
    bool hasAnyApps_ = false;
    int64_t fetchedAtMs_ = 0;
    int64_t attemptedAtMs_ = 0;
    std::string error_;
};

} // namespace service
} // namespace nativetv
