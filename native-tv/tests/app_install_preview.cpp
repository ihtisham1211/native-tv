#include "service/app_install_preview.hpp"

#include <cassert>
#include <iostream>

using namespace nativetv;
using namespace nativetv::service;

namespace {
Json makeInput() {
    Json input = Json::object();
    input.set("jobId", Json::text("fixture-job"));
    input.set("source", Json::text("catalog"));
    input.set("package", Json::text("com.example.app"));
    input.set("version", Json::text("1.0.0"));
    Json apps = Json::array();
    Json app = Json::object();
    app.set("id", Json::text("com.example.app"));
    app.set("title", Json::text("Fixture app"));
    apps.add(app);
    input.set("apps", apps);
    input.set("installedApps", Json::array());
    Json services = Json::array();
    services.add(Json::text("com.example.app.service"));
    input.set("services", services);
    input.set("rootRequired", Json::flag(true));
    input.set("pkgVetted", Json::flag(true));
    input.set("reelevate", Json::array());
    input.set("cpuMismatch", Json::flag(false));
    input.set("storeInstalled", Json::flag(false));
    input.set("elevationAvailable", Json::flag(true));
    input.set("freeBytes", Json::numeric(4096));
    input.set("packageBytes", Json::numeric(1024));
    input.set("sha256", Json::text(std::string(64, 'a')));
    return input;
}

Json build(const Json &input, bool &needsReview, bool &capacityOkay) {
    Json preview;
    std::string error;
    assert(AppInstallPreview::build(input, preview, needsReview, capacityOkay, error));
    assert(error.empty());
    return preview;
}
} // namespace

int main() {
    bool needsReview = true;
    bool capacityOkay = false;
    Json input = makeInput();
    Json preview = build(input, needsReview, capacityOkay);
    assert(!needsReview);
    assert(capacityOkay);
    assert(preview["needBytes"].number() == 2048);
    assert(preview["freeBytes"].number() == 4096);
    assert(preview["direction"].string() == "new");
    assert(preview["vetted"].boolean());
    assert(preview["rootRequired"].boolean());
    assert(preview["willElevate"].boolean() == false);
    assert(preview["apps"].dump() == input["apps"].dump());
    assert(preview["services"].dump() == input["services"].dump());

    Json installedApps = Json::array();
    Json installedApp = Json::object();
    installedApp.set("id", Json::text("com.example.app"));
    installedApp.set("version", Json::text("2.0.0"));
    installedApps.add(installedApp);
    input.set("installedApps", installedApps);
    preview = build(input, needsReview, capacityOkay);
    assert(needsReview);
    assert(preview["direction"].string() == "down");
    assert(preview["installedVersion"].string() == "2.0.0");

    installedApps = Json::array();
    installedApp.set("version", Json::text("0.9.0"));
    installedApps.add(installedApp);
    input.set("installedApps", installedApps);
    preview = build(input, needsReview, capacityOkay);
    assert(!needsReview);
    assert(preview["direction"].string() == "up");

    installedApps = Json::array();
    installedApp.set("version", Json::text("1.0"));
    installedApps.add(installedApp);
    input.set("installedApps", installedApps);
    preview = build(input, needsReview, capacityOkay);
    assert(!needsReview);
    assert(preview["direction"].string() == "same");

    input = makeInput();
    input.set("pkgVetted", Json::flag(false));
    preview = build(input, needsReview, capacityOkay);
    assert(needsReview);
    assert(!preview["vetted"].boolean());

    input.set("elevationAvailable", Json::flag(false));
    preview = build(input, needsReview, capacityOkay);
    assert(!needsReview);

    input = makeInput();
    input.set("source", Json::text("url"));
    input.set("pkgVetted", Json::flag(true));
    preview = build(input, needsReview, capacityOkay);
    assert(!preview["vetted"].boolean());

    input = makeInput();
    input.set("package", Json::text("com.example.Screensaver"));
    preview = build(input, needsReview, capacityOkay);
    assert(needsReview);
    assert(preview["isScreensaver"].boolean());

    input = makeInput();
    input.set("package", Json::text("com.example.bundle"));
    Json bundleApps = Json::array();
    Json bundleFirst = Json::object();
    bundleFirst.set("id", Json::text("com.example.first"));
    bundleApps.add(bundleFirst);
    Json bundleSecond = Json::object();
    bundleSecond.set("id", Json::text("com.example.second"));
    bundleApps.add(bundleSecond);
    input.set("apps", bundleApps);
    installedApps = Json::array();
    installedApp.set("id", Json::text("com.example.first"));
    installedApp.set("version", Json::text("3.2"));
    installedApps.add(installedApp);
    input.set("installedApps", installedApps);
    input.set("installedVersion", Json::text("99.0"));
    preview = build(input, needsReview, capacityOkay);
    assert(preview["installedVersion"].string() == "3.2");
    assert(preview["direction"].string() == "down");

    input = makeInput();
    input.set("reelevate", input["services"]);
    preview = build(input, needsReview, capacityOkay);
    assert(preview["willElevate"].boolean());

    input = makeInput();
    input.set("sha256", Json::text("bad"));
    Json invalidPreview;
    std::string error;
    assert(!AppInstallPreview::build(input, invalidPreview, needsReview, capacityOkay, error));
    assert(!error.empty());

    input = makeInput();
    input.set("package", Json::text("../outside"));
    assert(!AppInstallPreview::build(input, invalidPreview, needsReview, capacityOkay, error));
    assert(!error.empty());

    input = makeInput();
    input.set("freeBytes", Json::numeric(2047));
    preview = build(input, needsReview, capacityOkay);
    assert(!capacityOkay);
    assert(preview["needBytes"].number() == 2048);
    assert(preview["freeBytes"].number() == 2047);

    input.set("freeBytes", Json());
    preview = build(input, needsReview, capacityOkay);
    assert(capacityOkay);
    assert(!preview["freeBytes"].valid());

    input.set("packageBytes", Json::numeric(1024.5));
    assert(!AppInstallPreview::build(input, invalidPreview, needsReview, capacityOkay, error));
    assert(!error.empty());

    std::cout << "native app install preview assertions passed\n";
    return 0;
}
