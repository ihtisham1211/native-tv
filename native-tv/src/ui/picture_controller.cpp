#include "ui/application.hpp"

namespace nativetv {
void Application::clearPicture() {
    services_.cancel("home-now");
    services_.cancel("home-picture");
    state_.home.sourceGeneration = state_.home.pictureGeneration = 0;
    state_.home.pictureKey.clear();
    state_.home.picture = Json();
}
void Application::pictureResponse(const Response &response) {
    if (!active_ || state_.screen != Screen::Home ||
        !state_.catalogue["layout"]["picture"].boolean())
        return;
    auto &home = state_.home;
    unsigned expected =
        response.channel == "home-now" ? home.sourceGeneration : home.pictureGeneration;
    if (!expected || response.generation != expected)
        return;
    Json data = response.json();
    dirty_ = true;
    if (!response.ok() || !data.valid()) {
        clearPicture();
        return;
    }
    if (response.channel == "home-picture") {
        home.pictureGeneration = 0;
        home.picture = data;
        return;
    }
    home.sourceGeneration = 0;
    std::string kind = data["kind"].string();
    if (kind != "input" && kind != "channel" && kind != "app") {
        clearPicture();
        return;
    }
    Json arguments = fields({{"kind", Json::text(kind)}});
    if (kind == "input")
        arguments.set("port", data["port"]);
    if (kind == "app")
        arguments.set("app", data["appId"]);
    std::string key = arguments.dump();
    if (key != home.pictureKey) {
        home.picture = Json();
        home.pictureKey = key;
        services_.cancel("home-picture");
    }
    home.pictureGeneration = services_.read("home-picture", Operation::HomePicture, arguments);
}
} // namespace nativetv
