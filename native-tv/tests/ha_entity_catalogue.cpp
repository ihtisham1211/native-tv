#include "daemon/ha_entity_catalogue.hpp"
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

using nativetv::Json;
using namespace nativetv::daemon;

namespace {
void check(bool condition, const std::string &message) {
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}
}

int main(int argc, char **argv) {
    const Json categories = haEntityCategories();
    const Json catalogue = haEntityCatalogue();
    if (argc == 2 && std::string(argv[1]) == "--dump") {
        std::cout << catalogue.dump() << '\n';
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--filter") {
        const Json capabilities = Json::parse(argv[2]);
        if (!capabilities.isObject())
            return 2;
        std::cout << filterHaEntityCatalogue(catalogue, capabilities).published.dump()
                  << '\n';
        return 0;
    }
    check(categories.isArray() && categories.size() == 5,
          "Home Assistant categories changed");
    check(catalogue.isArray() && catalogue.size() == 114,
          "native entity catalogue does not match the current legacy catalogue");

    std::set<std::string> knownCategories;
    for (const Json &category : categories.items()) {
        check(category["id"].isString() && category["name"].isString() &&
                  category["desc"].isString(),
              "category metadata is incomplete");
        knownCategories.insert(category["id"].string());
    }
    check(knownCategories.count("controls") && knownCategories.count("oled") &&
              knownCategories.count("video") && knownCategories.count("system") &&
              knownCategories.count("diagnostics"),
          "required category is missing");

    std::set<std::string> seen;
    for (const Json &entity : catalogue.items()) {
        const Json id = entity["id"];
        const Json type = entity["type"];
        const Json name = entity["name"];
        const Json category = entity["cat"];
        check(id.isString() && type.isString() && name.isString() && category.isString(),
              "entity metadata is incomplete");
        check(knownCategories.count(category.string()) != 0,
              "entity references an unknown category: " + id.string());
        check(seen.insert(type.string() + "." + id.string()).second,
              "duplicate entity type and id: " + type.string() + "." + id.string());
    }

    check(catalogue[0]["id"].string() == "display_panel" &&
              catalogue[catalogue.size() - 1]["id"].string() == "smart_tips_in_settings",
          "entity catalogue ordering changed");
    std::cout << "native Home Assistant catalogue: " << catalogue.size()
              << " entities across " << categories.size() << " categories\n";
    return 0;
}
