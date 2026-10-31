#pragma once

#include "daemon/config.hpp"
#include "daemon/command_policy.hpp"
#include "daemon/app_install_upload_reservation.hpp"
#include "daemon/http_auth.hpp"
#include "daemon/hostname_cache.hpp"
#include "daemon/http_proxy.hpp"
#include "daemon/http_static_assets.hpp"
#include "service/admin_pin.hpp"
#include "service/app_catalog.hpp"
#include "service/app_package_inspector.hpp"
#include "service/app_package_fetcher.hpp"
#include "service/cpu_windows.hpp"
#include "service/home_content.hpp"
#include "service/home_discover.hpp"
#include "service/home_image_proxy.hpp"
#include "service/home_picture.hpp"
#include "service/home_open.hpp"
#include "service/home_recent_apps.hpp"
#include "service/network_rates.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nativetv {
namespace service {
class HistoryRecorder;
class PiccapStatus;
class ReleaseChecker;
struct BackgroundServiceInfo;
}
namespace daemon {
class LunaClient;
class LunaSubscription;
class EventLoop;
class ChildProcessRunner;
class AdBlockManager;
class GameFrameRate;
class Logger;
class ScreenTimeEnforcer;
class StateSubscriptions;
class DevtoolsFirewall;
class HomeScreenManager;
class HomeTakeoverLifecycleManager;
class ScreensaverManager;
class TvAppManager;
class ScreensaverControl;
class StatsCollector;
class HomeOrchestrator;
class HomeProbeGate;
class InstallJobs;
class SamRescanManager;
class AppTileManager;

struct HomePictureTrackingState {
    std::mutex mutex;
    std::map<std::string,
             std::pair<std::chrono::steady_clock::time_point, Json>> cache;
    std::map<std::string, Json> played;
    std::map<std::string, uint64_t> generations;
};

struct HomeNowTrackingState {
    std::mutex mutex;
    std::string appId;
    std::string observedAppId;
    int64_t since = 0;
};

struct HomeRecentHistoryState {
    std::mutex mutex;
    std::string path;
};

// The input a device tile just opened: its arrival in front is the tile, not
// the remote, so the foreground watcher skips that stamp.
struct HomeTileInputState {
    std::mutex mutex;
    std::string appId;
    int64_t at = 0;
};

class HttpFrontDoor {
  public:
    HttpFrontDoor(ServerConfig config, std::string version, std::string dataRoot,
                  std::string configPath, HttpProxy proxy, HttpProxy handoffProxy,
                  LunaClient &luna, ChildProcessRunner &children, std::string assetsRoot,
                  std::string procRoot = "/proc", std::string systemRoot = "/",
                  unsigned cpuWindowMs = 5000, Logger *logger = nullptr,
                  service::HistoryRecorder *historyRecorder = nullptr,
                  ScreenTimeEnforcer *screenTimeEnforcer = nullptr,
                  CommandPolicyExecutor *commandPolicy = nullptr,
                  GameFrameRate *gameFrameRate = nullptr,
                  StateSubscriptions *stateSubscriptions = nullptr,
                  AdBlockManager *adBlockManager = nullptr,
                  DevtoolsFirewall *devtoolsFirewall = nullptr,
                  ScreensaverManager *screensaverManager = nullptr,
                  HomeScreenManager *homeScreenManager = nullptr,
                  SamRescanManager *samRescanManager = nullptr,
                  AppTileManager *appTileManager = nullptr,
                  service::AppCatalogFetcher appCatalogFetcher = {},
                  service::AppPackageDownload appPackageFetcher = {},
                  HomeTakeoverLifecycleManager *homeTakeoverLifecycleManager = nullptr,
                  EventLoop *eventLoop = nullptr);
    ~HttpFrontDoor();
    void handle(HttpExchange &exchange) const;
    // A live state event: the next stats read is made afresh, and the cached
    // reads the event's group makes stale are dropped.
    void expireStats(const std::string &group) const;
    // What Node's private routes answer, for a start with no Node (see NodeFreeHooks).
    bool nodeFreeRestart(bool checkOnly, int &status, std::string &body, std::string &error) const;
    bool nodeFreeSetupContext(bool demo, Json &context, std::string &error) const;
    bool nodeFreeHistoryOverlay(int64_t fromMs, int64_t toMs, const Json &ids, Json &overlay,
                                std::string &error) const;
    // The retained telemetry payload for Home Assistant from one system read
    // and LG's settings rows (legacy lib/mqtt-state.js publishPayload).
    // Blocking: run on a worker. Invalid when the read failed.
    Json composeMqttTelemetry(const Json &system, const Json &extra) const;
    // The device record, picture modes and sound outputs of the last stats read, for
    // Home Assistant's discovery; invalid before a read has run.
    Json mqttDiscoveryInputs() const;
    // HDMI tile titles on webOS 9's Home follow the CEC names of what is
    // connected. EIM cannot be subscribed to, so this is called at start, when
    // Home opens and when the TV switches on, never in standby.
    void refreshHomeInputTitles() const;
    // What lib/home.js still did around the daemon's own Home routes (start-up
    // sequence, the wake open, auto launch following the screen), or null unless
    // TVWEB_NATIVE_HOME_ORCH=1.
    HomeOrchestrator *homeOrchestrator() const;
    // Whether Native Tv Home is offered here: the webOS version and the native
    // client's probe. `probe` false uses a verdict the probe already gave (or
    // runs it when there is none); true runs it again.
    Json homeSupported(bool probe) const;
    // Binds (or removes) the stub over a switched-off background service's
    // programs, and its jail's copy where it has one.
    bool setServiceStubs(const service::BackgroundServiceInfo &info, const std::string &id,
                         bool stubbed, std::string &error) const;
    // What the server did once at start that the daemon now owns: restage the
    // active screen saver from the current assets, adopt a saver an earlier
    // run launched, and read the Home tile titles. Blocks; run on a worker.
    void runStartupTasks() const;
    // After an install or uninstall: has sam read the apps again (restarting it if it
    // will not), when the home ribbon would otherwise miss the change.
    void refreshLauncher() const;
    // On an OLED48C5PUA (webOS 10.3) the preload manager has every app enabled
    // again at each switch-on while configd still holds isEnabled false, so the
    // owner's apps are told off again at every switch-on and after boot.
    void resendPreload() const;
    void reapplyPreloadAfterBoot() const;
    // Live NTP and syslog status for /api/update. The forwarders belong to the
    // daemon's main scope, so it hands the front door their accessors.
    void setStatusProviders(std::function<Json()> ntp, std::function<Json()> syslog);
    void setTvAppManager(TvAppManager *manager) { tvAppManager_ = manager; }
    // Whether GitHub has a newer release: the Server tab's Check button, and
    // the opt-in daily check (checkReleaseIfDue, run hourly).
    void setReleaseChecker(service::ReleaseChecker *checker) { releaseChecker_ = checker; }
    bool checkReleaseIfDue(std::string &error) const;
    // Where the MQTT bridge stands, when the daemon owns it: {state, detail,
    // sinceMs, lastPublishMs, latencyMs}. Without it /api/settings asks Node.
    void setMqttStatusProvider(std::function<Json()> provider) { mqttStatus_ = std::move(provider); }
    // Clears the retained topics the MQTT client published and disconnects it;
    // returns how many it cleared. Called on uninstall, from Node.
    void setMqttForget(std::function<size_t()> forget) { mqttForget_ = std::move(forget); }
    // Switching and triggering the screen saver; null until the managers it
    // needs are present.
    ScreensaverControl *screensaverControl() const;
    // The dashboard tile's install script. Refused while a package installs,
    // as the script drives the same developer install service.
    Json runTvApp(const std::string &action, const std::string &appDir = std::string()) const;
    void handlePhoneHandoff(HttpExchange &exchange) const;
    void handleLanIcon(HttpExchange &exchange) const;
    bool createDailyBackupIfDue(std::string &error) const;
    Json collectMqttLgSettings() const;
    Json executeMqttControl(const Json &payload) const;
    Json openLgHomeFromRemoteInput() const;
    Json returnNativeTvHomeFromRemoteInput() const;
    bool reconcileNativeRemoteKeys(bool &wrote, std::string &error) const;
    // LG's own values put back and the saved key map cleared (tvwebctl restore-keys,
    // and Node's restore while it delegates).
    bool restoreNativeRemoteKeys(std::string &error) const;

  private:
    bool setupHandoffAction(HttpExchange &exchange, const std::string &expectedAction,
                            const Json *providedPayload = nullptr) const;
    void handleHdmiSeen(HttpExchange &exchange) const;
    void handleLastApp(HttpExchange &exchange) const;
    void handleMqttForget(HttpExchange &exchange) const;
    void handleRemoteKeysApply(HttpExchange &exchange) const;
    void handleAppsState(HttpExchange &exchange) const;
    void handleAppElevation(HttpExchange &exchange) const;
    void handleAppElevationRun(HttpExchange &exchange) const;
    void handleInstalledApps(HttpExchange &exchange) const;
    void handleAppInstallFetch(HttpExchange &exchange) const;
    void handleAppInstallUrl(HttpExchange &exchange) const;
    void buildAppInstallPreview(HttpExchange &exchange) const;
    void readAppInstallSpace(HttpExchange &exchange) const;
    void cacheAppInstallStartSnapshot(const Json &response) const;
    // TVWEB_NATIVE_INSTALL: the daemon runs the install jobs; otherwise Node does.
    bool nativeInstallEnabled() const;
    InstallJobs *installJobs() const;
    bool startAppInstall(const Json &payload, int &status, Json &response,
                         std::string &error) const;
    bool startAppUpload(const std::string &file, int &status, Json &response,
                        std::string &error) const;
    bool runAppInstallAction(const std::string &action, const Json &payload, bool validated,
                             int &status, Json &response, std::string &error) const;
    void handleAppInstallAction(HttpExchange &exchange,
                                const std::string &action) const;
    void sendAppInstallStatus(HttpExchange &exchange) const;
    void handleAppUpload(HttpExchange &exchange) const;
    void handleAppInstallRun(HttpExchange &exchange) const;
    void handleInstallerJob(HttpExchange &exchange) const;
    void setNativeBackupConfig(HttpExchange &exchange) const;
    void validateNativeBackupRestore(HttpExchange &exchange) const;
    void setNativePendingBackupApps(HttpExchange &exchange) const;
    void planNativeBackupApps(HttpExchange &exchange) const;
    void restoreNativeBackupLg(HttpExchange &exchange) const;
    // POST /api/backup: the whole restore, from the accepted request body.
    void handleBackupRestore(HttpExchange &exchange, const std::string &body) const;
    void setNativeDailyBackup(HttpExchange &exchange) const;
    void sendAppIcon(HttpExchange &exchange) const;
    // The admin PIN (service/admin_pin.hpp): its routes, and the check the risky routes share.
    bool adminAllowed(HttpExchange &exchange) const;
    void sendAdminStatus(HttpExchange &exchange) const;
    // unlock opens an admin session; login is the same check for a browser with no token,
    // and answers the token as well.
    void handleAdminUnlock(HttpExchange &exchange, bool login) const;
    void handleAdminLock(HttpExchange &exchange) const;
    void handleAdminPin(HttpExchange &exchange) const;
    void setupAdminPin(HttpExchange &exchange, const Json &payload) const;
    std::string activeHandoffUrl() const;
    std::string collectCpuMetrics() const;
    Json withStatsWindows(const Json &system, const std::string &reader) const;
    bool collectNativeScreenTimeStatus(Json &response, std::string &error) const;
    void sendLogs(HttpExchange &exchange) const;
    void sendHdmiInputs(HttpExchange &exchange) const;
    void sendServiceMenuState(HttpExchange &exchange) const;
    void sendScreensaverList(HttpExchange &exchange) const;
    Json serverStatus() const;
    // /api/stats built here rather than by Node; null until the managers it
    // needs are present.
    StatsCollector *statsCollector() const;
    // What the main Home profile is called until the owner renames it: the TV's own series.
    std::string mainProfileName() const;
    void dispatch(HttpExchange &exchange) const;
    service::PiccapStatus *piccapStatus() const;
    // The cached reverse-DNS name of an address; a stale one is looked up behind
    // the answer, so a caller never waits on DNS.
    std::string localHostname(const std::string &address) const;
    // The TV's model: config.json's device.model, else what the TV reports (kept
    // once it has answered), as the server filled it in at start.
    std::string tvModelName() const;
    // What /api/settings and the backup export need beyond the config file: the
    // MQTT bridge's standing, the Home Assistant catalogue, the Home layout and
    // the display version. False only if the standing is Node's and Node is down.
    bool collectSettingsContext(Json &context, std::string &error, bool withStatus = true) const;
    // The fields of GET /api/apps that are not the app list itself: tile hiding,
    // services, preloading, Quick Access and the Home screen's parts and tiles.
    Json appsRuntimeContext(const Json &appsReply, const Json &launchPointsReply) const;
    // The release version with the matching git build commit deploy.sh wrote.
    std::string displayVersion() const;
    // POST /api/home/beat: the Home page's heartbeat, answered with the state
    // it reloads on (build, layout and wallpapers).
    void sendHomeBeat(HttpExchange &exchange) const;
    // POST /api/homescreen: LG's Home screen parts and HDMI input tiles.
    void setHomeScreen(HttpExchange &exchange) const;
    // One action of the Home screen manager, as the internal bridge runs it.
    Json homeScreenAction(const Json &command) const;
    // Restarts the server through tvwebctl after delayMs, as the port change
    // and setup need; false (with the reason) while a package is installing or
    // when tvwebctl is missing.
    bool restartServer(unsigned delayMs, std::string &error) const;
    // Runs one native app tile operation in process (the internal route's
    // validation, command policy and SAM refresh) and returns its JSON reply.
    Json appTileOperation(const std::string &action, int &status) const;
    // The Server tab's switches, each answered with serverStatus(). Returns
    // false for an action that is not one of them.
    bool serverSettingControl(const std::string &action, const Json &value,
                              Json &response) const;
    // config.json as it is now. config_ is read once at start, so settings
    // written since (by this daemon or by hand) show only through this.
    Json currentConfigDocument() const;
    void sendOledCare(HttpExchange &exchange) const;
    void sendPrivacyProjection(HttpExchange &exchange) const;
    void setSshKeys(HttpExchange &exchange) const;
    void sendBackgroundServices(HttpExchange &exchange) const;
    void setBackgroundService(HttpExchange &exchange) const;
    bool applyBackgroundServiceState(
        const service::BackgroundServiceInfo &info, const std::string &id,
        bool disabled, Json &result, std::string &error) const;
    void setNativeDevtools(HttpExchange &exchange) const;
    void setNativeScreensaver(HttpExchange &exchange) const;
    void setNativeSamRescan(HttpExchange &exchange) const;
    void setNativeAppTileOperation(HttpExchange &exchange) const;
    void setNativeAppUninstall(HttpExchange &exchange) const;
    void setNativeAppSideload(HttpExchange &exchange) const;
    void setNativePublicAppsRepos(HttpExchange &exchange) const;
    void setNativeAppsRepos(HttpExchange &exchange) const;
    void checkNativeAppsRepository(HttpExchange &exchange) const;
    void findNativeAppPackage(HttpExchange &exchange) const;
    void downloadNativeAppPackage(HttpExchange &exchange) const;
    void inspectNativeAppPackage(HttpExchange &exchange) const;
    void sendApps(HttpExchange &exchange) const;
    void sendAppCatalog(HttpExchange &exchange) const;
    void sendHomeChannels(HttpExchange &exchange) const;
    void sendHomeStatus(HttpExchange &exchange) const;
    void sendHomeCatalogue(HttpExchange &exchange, bool view) const;
    Json homeCatalogueResponse(bool view) const;
    Json homeCatalogueAppsSnapshot() const;
    Json homeCatalogueDevicesSnapshot() const;
    void sendHomeGuide(HttpExchange &exchange) const;
    void sendHomeNow(HttpExchange &exchange) const;
    void sendHomePicture(HttpExchange &exchange) const;
    void ensureHomePictureTracking() const;
    void sendHomeContent(HttpExchange &exchange, const std::string &kind) const;
    void sendHomeDiscover(HttpExchange &exchange) const;
    void sendHomeImage(HttpExchange &exchange) const;
    void sendHomeWallpaper(HttpExchange &exchange) const;
    void handleHomeWallpaperUpload(HttpExchange &exchange) const;
    void handleHomeWallpapersRemove(HttpExchange &exchange) const;
    void sendHomeRecentApps(HttpExchange &exchange) const;
    void sendHomeRecentAppImage(HttpExchange &exchange) const;
    void handleHomeRecentAppsPost(HttpExchange &exchange) const;
    void handleHomeLayoutPost(HttpExchange &exchange) const;
    void handleHomeStartLayoutPost(HttpExchange &exchange) const;
    void handleHomeStartupPost(HttpExchange &exchange) const;
    bool updateHomeStartingNotice(const Json &home) const;
    HomeProbeGate *homeProbeGate() const;
    // Native Tv Home gave LG's Home back (crash loop or probe) and stays out of it.
    bool homeHalted() const;
    bool syncHomeEarlyService(bool hasStartingNotice,
                              std::string *error = nullptr) const;
    void handleHomeEnablePost(HttpExchange &exchange) const;
    void handleHomeDisablePost(HttpExchange &exchange) const;
    void handleHomeProfilePost(HttpExchange &exchange) const;
    void handleHomeOpenPost(HttpExchange &exchange) const;
    Json openLgHomeEscape(int &status) const;
    Json returnNativeTvHomeAfterForegroundChange(
        const std::string &observedAppId) const;
    void queueHomeForegroundReturn(const std::string &appId) const;
    void persistHomeConfig(HttpExchange &exchange) const;
    void persistHomeRecent(HttpExchange &exchange) const;
    void syncHomeAutoLaunch(HttpExchange &exchange) const;
    Json synchronizeHomeAutoLaunch(bool escaped) const;
    Json recordHomeRecent(const std::string &row, const std::string &id) const;
    service::HomeRecentApps queryHomeRecentApps() const;
    Json homeChannelSnapshot(bool &tunerAvailable) const;
    void setNativeHomeScreen(HttpExchange &exchange) const;
    void setNativeHomeInput(HttpExchange &exchange, const Json &payload) const;
    void invalidateOledCareCache() const;
    void invalidatePrivacyCache() const;
    void sendGameSettings(HttpExchange &exchange) const;
    void sendGameFrameRate(HttpExchange &exchange) const;
    void sendLgSettings(HttpExchange &exchange) const;
    void handleSavedPage(HttpExchange &exchange) const;
    Json collectLgSettingsData(const std::vector<std::string> &sections) const;
    Json lgSettingsInputNames() const;
    void invalidateLgSettingsCache() const;
    void setControl(HttpExchange &exchange) const;
    void setConsentControl(HttpExchange &exchange, const Json &value) const;
    void setAcrControl(HttpExchange &exchange, const Json &value) const;
    void setLgAdsControl(HttpExchange &exchange, const Json &value) const;
    Json applyConsentChange(const std::string &key, bool enabled) const;
    void invalidateNodeControlCaches() const;
    void setLimitAdTrackingControl(HttpExchange &exchange, const Json &value) const;
    void clearAdCookiesControl(HttpExchange &exchange) const;
    void resetAdIdControl(HttpExchange &exchange) const;
    void setAdBlockCustom(HttpExchange &exchange) const;
    void setPiccapControl(HttpExchange &exchange, const Json &value) const;
    void setQuickAccess(HttpExchange &exchange) const;
    void setRemoteKey(HttpExchange &exchange) const;
    void setPreloadToggle(HttpExchange &exchange) const;
    void sendDashboardSettings(HttpExchange &exchange) const;
    void setDashboardSettings(HttpExchange &exchange) const;
    bool createBackup(Json &backup, const Json *settingsContext = nullptr) const;
    bool createDailyBackup(const std::string &name, std::string &error) const;
    void sendBackup(HttpExchange &exchange) const;
    Json hdmiInputDevices() const;
    Json hdmiReceiverMap() const;

    ServerConfig config_;
    mutable std::mutex screenTimeSettingsMutex_;
    mutable Json screenTimeSettings_;
    std::string version_;
    mutable std::mutex appCatalogReposMutex_;
    mutable std::mutex appReposMutationMutex_;
    mutable Json appCatalogRepos_;
    mutable service::AppCatalog appCatalog_;
    mutable std::mutex homeChannelsMutex_;
    mutable Json homeChannelsCache_;
    mutable bool homeChannelsCacheValid_ = false;
    mutable std::chrono::steady_clock::time_point homeChannelsCachedAt_{};
    mutable std::mutex homeCatalogueMutex_;
    mutable Json homeCatalogueAppsCache_;
    mutable Json homeCatalogueDevicesCache_;
    mutable bool homeCatalogueAppsCacheValid_ = false;
    mutable bool homeCatalogueDevicesCacheValid_ = false;
    mutable std::chrono::steady_clock::time_point homeCatalogueAppsCachedAt_{};
    mutable std::chrono::steady_clock::time_point homeCatalogueDevicesCachedAt_{};
    mutable std::mutex homeGuideMutex_;
    mutable Json homeGuideCache_;
    mutable bool homeGuideCacheValid_ = false;
    mutable std::chrono::steady_clock::time_point homeGuideCachedAt_{};
    mutable std::mutex homePictureMutex_;
    mutable std::shared_ptr<HomePictureTrackingState> homePictureState_;
    mutable std::shared_ptr<LunaSubscription> homePictureVideoSubscription_;
    mutable std::shared_ptr<HomeNowTrackingState> homeNowState_;
    mutable std::shared_ptr<HomeRecentHistoryState> homeRecentHistoryState_;
    mutable std::shared_ptr<HomeTileInputState> homeTileInputState_;
    mutable std::shared_ptr<std::atomic<bool>> homeEscapeActive_;
    mutable std::shared_ptr<std::atomic<bool>> homeEscapeReturnPending_;
    mutable std::mutex homeRecentAppsMutex_;
    mutable std::mutex homeConfigMutex_;
    mutable std::mutex homeAutoLaunchMutex_;
    mutable std::mutex homeOperationMutex_;
    mutable service::HomeRecentApps homeRecentAppsCache_;
    mutable bool homeRecentAppsCacheValid_ = false;
    mutable std::atomic<bool> homeWallpaperUploadActive_{false};
    mutable std::atomic<bool> samRestartedThisRun_{false};
    service::AppPackageDownload appPackageFetcher_;
    std::string dataRoot_;
    mutable service::AdminPin adminPin_;
    mutable service::HomeContent homeContent_;
    mutable service::HomeDiscover homeDiscover_;
    mutable service::HomeImageProxy homeImageProxy_;
    std::string configPath_;
    std::string hdmiSeenPath_;
    std::string lastAppPath_;
    std::string hiddenAppsPath_;
    std::string tileHidingFlagPath_;
    std::string elevatedAppsPath_;
    std::string installJobPath_;
    std::string proxySecret_;
    mutable std::mutex hdmiSeenMutex_;
    mutable std::mutex lastAppMutex_;
    std::function<size_t()> mqttForget_;
    mutable bool mqttLastAppLoaded_ = false;
    mutable std::string mqttLastApp_;
    mutable std::string mqttLastAppId_;
    mutable std::mutex utcOffsetMutex_;
    mutable long long utcOffsetReadAt_ = 0;
    mutable int utcOffsetMinutes_ = 0;
    mutable std::mutex appsStateMutex_;
    mutable std::mutex elevatedAppsMutex_;
    mutable std::mutex appElevationRunMutex_;
    mutable std::mutex appInstallRunMutex_;
    mutable std::mutex appInstallActionMutex_;
    mutable std::mutex appInstallDownloadsMutex_;
    mutable std::map<std::string, std::shared_ptr<std::atomic<bool>>>
        appInstallDownloads_;
    mutable AppInstallUploadReservation appInstallUploadReservation_;
    mutable std::mutex installJobMutex_;
    mutable Json appInstallSnapshot_;
    mutable bool appInstallSnapshotReady_ = false;
    mutable std::mutex pendingBackupAppsMutex_;
    mutable std::mutex dailyBackupMutex_;
    HttpProxy proxy_;
    HttpProxy handoffProxy_;
    LunaClient &luna_;
    ChildProcessRunner &children_;
    Logger *logger_;
    service::HistoryRecorder *historyRecorder_;
    ScreenTimeEnforcer *screenTimeEnforcer_;
    CommandPolicyExecutor *commandPolicy_;
    GameFrameRate *gameFrameRate_;
    StateSubscriptions *stateSubscriptions_;
    AdBlockManager *adBlockManager_;
    DevtoolsFirewall *devtoolsFirewall_;
    ScreensaverManager *screensaverManager_;
    HomeScreenManager *homeScreenManager_;
    HomeTakeoverLifecycleManager *homeTakeoverLifecycleManager_;
    SamRescanManager *samRescanManager_;
    AppTileManager *appTileManager_;
    EventLoop *eventLoop_;
    TvAppManager *tvAppManager_ = nullptr;
    mutable std::unique_ptr<ScreensaverControl> screensaverControl_;
    mutable std::once_flag screensaverControlOnce_;
    mutable std::unique_ptr<StatsCollector> statsCollector_;
    mutable std::once_flag homeOrchestratorOnce_;
    mutable std::unique_ptr<HomeOrchestrator> homeOrchestrator_;
    mutable std::once_flag homeProbeGateOnce_;
    mutable std::unique_ptr<HomeProbeGate> homeProbeGate_;
    mutable std::once_flag statsCollectorOnce_;
    mutable std::unique_ptr<service::PiccapStatus> piccapStatus_;
    mutable std::once_flag piccapStatusOnce_;
    mutable std::unique_ptr<InstallJobs> installJobs_;
    mutable std::once_flag installJobsOnce_;
    mutable HostnameCache hostnames_;
    mutable std::mutex powerOnMutex_;
    mutable Json powerOnFacts_;
    mutable long long powerOnReadAt_ = 0;
    mutable std::string powerOnState_;
    mutable std::mutex homeTitleRefreshMutex_;
    mutable std::mutex tvModelMutex_;
    mutable std::string tvModel_;
    mutable std::mutex liveConfigMutex_;
    mutable Json liveConfig_;
    mutable std::string liveConfigStamp_;
    std::function<Json()> ntpStatus_;
    service::ReleaseChecker *releaseChecker_ = nullptr;
    std::function<Json()> mqttStatus_;
    std::function<Json()> syslogStatus_;
    std::chrono::steady_clock::time_point startedAt_ = std::chrono::steady_clock::now();
    long long startedAtMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string assetsRoot_;
    HttpStaticAssets staticAssets_;
    std::string procRoot_;
    std::string systemRoot_;
    unsigned cpuWindowMs_;
    mutable service::SystemCpuWindows statsCpuWindows_;
    mutable service::SystemNetworkRates statsNetworkRates_;
    mutable std::mutex cpuMetricsMutex_;
    mutable std::condition_variable cpuMetricsCondition_;
    mutable bool cpuMetricsInFlight_ = false;
    mutable uint64_t cpuMetricsGeneration_ = 0;
    mutable std::string cpuMetricsResponse_;
    mutable std::mutex hdmiReceiverMutex_;
    mutable std::condition_variable hdmiReceiverCondition_;
    mutable bool hdmiReceiverLoading_ = false;
    mutable bool hdmiReceiverLoaded_ = false;
    mutable Json hdmiReceiverMap_;
    mutable std::mutex handoffMutex_;
    mutable std::mutex volumeControlMutex_;
    mutable std::mutex muteControlMutex_;
    mutable std::mutex piccapControlMutex_;
    mutable std::mutex remoteInputControlMutex_;
    mutable std::mutex notificationControlMutex_;
    mutable bool mediaAcbAbsent_ = false;
    mutable std::mutex screenOffControlMutex_;
    mutable std::mutex screenOnControlMutex_;
    mutable std::mutex powerControlMutex_;
    mutable std::mutex inputControlMutex_;
    mutable std::mutex systemSettingsControlMutex_;
    mutable std::mutex remoteKeysMutex_;
    mutable std::mutex oledControlMutex_;
    mutable std::mutex oledCareCacheMutex_;
    mutable Json oledCareCache_;
    mutable bool oledCareCacheValid_ = false;
    mutable std::chrono::steady_clock::time_point oledCareCachedAt_{};
    mutable std::mutex privacyCacheMutex_;
    mutable Json privacyCache_;
    mutable bool privacyCacheValid_ = false;
    mutable std::chrono::steady_clock::time_point privacyCachedAt_{};
    mutable std::mutex lgSettingsCacheMutex_;
    mutable Json lgSettingsCacheSettings_;
    mutable Json lgSettingsCacheDescriptions_;
    mutable Json lgSettingsCacheDescriptionCoverage_;
    mutable Json lgSettingsCacheDimensions_;
    mutable std::map<std::string, std::chrono::steady_clock::time_point>
        lgSettingsCategoryCachedAt_;
    mutable std::mutex inputNameCacheMutex_;
    mutable Json inputNameCache_;
    mutable bool inputNameCacheValid_ = false;
    mutable std::chrono::steady_clock::time_point inputNameCachedAt_;
    mutable std::mutex applicationControlMutex_;
    mutable std::mutex quickAccessMutex_;
    mutable std::mutex preloadMutex_;
    mutable std::mutex quickAccessInputMutex_;
    mutable Json quickAccessInputDevices_ = Json::array();
    mutable std::chrono::steady_clock::time_point quickAccessInputScannedAt_;
    mutable bool quickAccessInputScanComplete_ = false;
    mutable std::string handoffCode_;
    mutable std::string retiredHandoffCode_;
    mutable std::chrono::steady_clock::time_point handoffExpires_;
};
} // namespace daemon
} // namespace nativetv
