#include "service/privacy_consent.hpp"
#include "domain/say.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace nativetv {
namespace service {
namespace {
const char *const kNamedSettable[] = {
    "acrAllowed", "acrGdprAllowed", "acrAdAllowed", "customAdAllowed",
    "customadsAllowed", "cookiesAllowed", "thirdPartySharingAllowed",
    "additionalDataAllowed", "remoteDiagAllowed", "voiceAllowed", "voice2Allowed"
};

std::vector<std::string> stringItems(const Json &value);

// Each text sits beside its say::msg() key, for scripts/check-strings.py.
struct ConsentLabel {
    const char *key;
    const char *labelKey;
    const char *label;
    const char *detailKey;
    const char *detail;
};

const ConsentLabel kConsentLabels[] = {
    {"acrAllowed", "srv.consent.acrAllowed", "Screen content recognition",
     "srv.consent.acrAllowed.detail", "Lets LG identify what is on your screen to profile your viewing"},
    {"acrGdprAllowed", "srv.consent.acrGdprAllowed", "Screen recognition (GDPR consent)",
     "srv.consent.acrGdprAllowed.detail", "The EU consent record for screen content recognition"},
    {"acrAdAllowed", "srv.consent.acrAdAllowed", "Ads based on what you watch",
     "srv.consent.acrAdAllowed.detail", "Uses recognised screen content to target advertising"},
    {"customAdAllowed", "srv.consent.customAdAllowed", "Personalised advertising",
     "srv.consent.customAdAllowed.detail", "Tailors the ads shown on your TV to you"},
    {"customadsAllowed", "srv.consent.customadsAllowed", "Personalised advertising (secondary flag)",
     "srv.consent.customadsAllowed.detail", "A second personalised-advertising consent record"},
    {"cookiesAllowed", "srv.consent.cookiesAllowed", "Advertising cookies",
     "srv.consent.cookiesAllowed.detail", "Stores cookies used for ad tracking"},
    {"thirdPartySharingAllowed", "srv.consent.thirdPartySharingAllowed", "Sharing your data with other companies",
     "srv.consent.thirdPartySharingAllowed.detail", "Passes your usage data to third parties"},
    {"additionalDataAllowed", "srv.consent.additionalDataAllowed", "Additional usage data",
     "srv.consent.additionalDataAllowed.detail", "Extra analytics beyond what the TV needs to work"},
    {"remoteDiagAllowed", "srv.consent.remoteDiagAllowed", "Remote diagnostics upload",
     "srv.consent.remoteDiagAllowed.detail", "Lets LG collect and upload diagnostic reports from your TV"},
    {"voiceAllowed", "srv.consent.voiceAllowed", "Voice recordings",
     "srv.consent.voiceAllowed.detail", "Allows voice data to be collected and processed"},
    {"voice2Allowed", "srv.consent.voice2Allowed", "Voice recordings (secondary flag)",
     "srv.consent.voice2Allowed.detail", "A second voice-data consent record"}
};

const ConsentLabel kDocumentLabels[] = {
    {"S_DPA", "srv.consent.dpa", "Sharing viewing data with data partners",
     "srv.consent.dpa.detail", "Lets LG and Alphonso, its screen recognition partner, pass viewing and device information to other companies for ad measurement and analytics"},
    {"S_TAK", "srv.consent.tak", "Who.Where.What?",
     "srv.consent.tak.detail", "LG's content discovery, run with TheTake: identifies people, places and products in what is on screen, using viewing information that can include captured images of it"},
    {"S_MKT", "srv.consent.mkt", "Marketing messages",
     "srv.consent.mkt.detail", "Lets LG show marketing pop-ups and notifications on the TV: special offers and news about its content and services"}
};

bool locked(const std::string &key) {
    return key == "generalTermsAllowed" || key == "networkAllowed" ||
           key == "firstUseAllowed" || key == "allAllowed";
}

bool hidden(const std::string &key) {
    return key == "chpAllowed";
}

const ConsentLabel *consentLabel(const std::string &key) {
    for (const ConsentLabel &label : kConsentLabels)
        if (key == label.key) return &label;
    return nullptr;
}

// The group of a flag that an agreement describes (CONSENT_BY_DOCUMENT's third item).
std::string documentGroup(const std::string &id) {
    return id == "S_DPA" ? "analytics" : id == "S_TAK" ? "watching" : "advertising";
}

const ConsentLabel *documentLabel(const std::string &id) {
    for (const ConsentLabel &label : kDocumentLabels)
        if (id == label.key) return &label;
    return nullptr;
}

std::string consentGroup(const std::string &key) {
    static const std::map<std::string, std::string> groups = {
        {"customAdAllowed", "advertising"}, {"customadsAllowed", "advertising"},
        {"cookiesAllowed", "advertising"}, {"acrAdAllowed", "advertising"},
        {"acrAllowed", "watching"}, {"acrGdprAllowed", "watching"},
        {"voiceAllowed", "watching"}, {"voice2Allowed", "watching"},
        {"additionalDataAllowed", "analytics"}, {"remoteDiagAllowed", "analytics"},
        {"thirdPartySharingAllowed", "analytics"}, {"acrOnAllowed", "watching"},
        {"marketingOnAllowed", "advertising"}, {"shoppingOnAllowed", "advertising"},
        {"networkAllowed", "platform"}, {"generalTermsAllowed", "platform"},
        {"firstUseAllowed", "platform"}, {"allAllowed", "platform"}
    };
    const auto found = groups.find(key);
    return found == groups.end() ? "unknown" : found->second;
}

std::string consentName(const std::string &key) {
    if (key == "networkAllowed") return say::msg("srv.consent.name.networkAllowed", "Network use");
    if (key == "marketingOnAllowed") return say::msg("srv.consent.name.marketingOnAllowed", "Marketing");
    if (key == "shoppingOnAllowed") return say::msg("srv.consent.name.shoppingOnAllowed", "Shopping on live TV");
    if (key == "generalTermsAllowed") return say::msg("srv.consent.name.generalTermsAllowed", "Terms of Use and Privacy Policy");
    if (key == "acrOnAllowed") return say::msg("srv.consent.name.acrOnAllowed", "Screen recognition (master consent)");
    if (key == "allAllowed") return say::msg("srv.consent.name.allAllowed", "Select All");
    return "";
}

std::string lockedDetail(const std::string &key) {
    if (key == "generalTermsAllowed") return say::msg("srv.consent.locked.generalTerms", "Acceptance of the terms themselves.");
    if (key == "networkAllowed") return say::msg("srv.consent.locked.network", "Acceptance of network use.");
    if (key == "firstUseAllowed") return say::msg("srv.consent.locked.firstUse", "Part of first-boot setup.");
    if (key == "allAllowed")
        return say::msg("srv.consent.locked.all", "The Select-All. Read-only because whether writing it cascades to the other flags is untested.");
    return "";
}

std::string listText(const std::vector<std::string> &items) {
    return say::list(items);
}

Json readAgreementGroups(const Json &licenseMapping, bool &foundAny) {
    Json groups = Json::object();
    foundAny = false;
    const Json entries = licenseMapping["eulaMappingList"]["eulaInfo"];
    if (!entries.isArray()) return groups;
    for (const Json &entry : entries.items()) {
        const Json settingKey = entry["settingKey"];
        if (!settingKey.isString() || settingKey.string().empty()) continue;
        foundAny = true;
        // Node uses `mandatory || generalSelectAll`; an empty array is truthy
        // there, so the fallback is used only when mandatory is absent/null.
        const Json mandatory = entry["mandatory"];
        const Json idsValue = mandatory.isArray() ? mandatory : entry["generalSelectAll"];
        std::vector<std::string> ids = stringItems(idsValue);
        std::sort(ids.begin(), ids.end());
        Json list = Json::array();
        for (const std::string &id : ids) list.add(Json::text(id));
        groups.set(settingKey.string(), list);
    }
    return groups;
}

Json groupRows() {
    Json groups = Json::array();
    // id, name key, name, note key, note ("" for none).
    const char *rows[][5] = {
        {"advertising", "srv.consent.group.advertising", "Advertising", "", ""},
        {"watching", "srv.consent.group.watching", "What the TV watches and hears", "", ""},
        {"analytics", "srv.consent.group.analytics", "Analytics and sharing", "", ""},
        {"services", "srv.consent.group.services", "LG services", "", ""},
        {"unknown", "srv.consent.group.unknown", "No published description",
         "srv.consent.group.unknown.note", "The TV records these, but LG publishes nothing about them."},
        {"platform", "srv.consent.group.platform", "Set on the TV itself",
         "srv.consent.group.platform.note",
         "Acceptance records rather than collection choices. Changed in the TV's own menus, under Settings › General › About This TV › User Agreements."}
    };
    for (const auto &row : rows) {
        Json item = Json::array();
        item.add(Json::text(row[0]));
        item.add(Json::text(say::msg(row[1], row[2])));
        if (*row[4]) item.add(Json::text(say::msg(row[3], row[4])));
        groups.add(item);
    }
    return groups;
}

std::vector<std::string> readStrings(const Json &value) {
    std::vector<std::string> result;
    if (!value.isArray()) return result;
    for (const Json &item : value.items())
        if (item.isString()) result.push_back(item.string());
    return result;
}

Json describeUnlabelled(const std::string &key, bool enabled, const Json &groups,
                        bool mapFound) {
    Json row = Json::object();
    row.set("key", Json::text(key));
    row.set("enabled", Json::flag(enabled));
    row.set("settable", Json::flag(privacyConsentSettable(key, groups)));
    row.set("group", Json::text(consentGroup(key)));
    const std::string name = consentName(key);
    if (!name.empty()) row.set("label", Json::text(name));
    const Json docs = groups[key];
    if (docs.valid()) row.set("documents", docs);
    if (key == "shoppingOnAllowed") {
        row.set("detail", Json::text(say::msg("srv.consent.shoppingOn.detail", "Read by the service that places ads and offers over live TV, alongside personalised advertising")));
        row.set("described", Json::flag(true));
        return row;
    }
    const std::string lockedText = lockedDetail(key);
    if (!lockedText.empty()) {
        row.set("detail", Json::text(lockedText));
        return row;
    }
    if (!docs.valid()) {
        row.set("detail", Json::text(mapFound
            ? "Tied to no agreement on this firmware."
            : "This TV publishes no agreement mapping, so there is nothing to go on."));
        return row;
    }
    std::vector<std::string> peers;
    const std::vector<std::string> myDocs = readStrings(docs);
    for (const std::string &candidateKey : groups.keys()) {
        if (key == candidateKey) continue;
        const ConsentLabel *candidate = consentLabel(candidateKey);
        if (!candidate) continue;
        const std::vector<std::string> peerDocs = readStrings(groups[candidateKey]);
        if (!peerDocs.empty() && peerDocs == myDocs)
            peers.push_back(std::string("\"") + say::msg(candidate->labelKey, candidate->label) + "\"");
    }
    if (peers.empty()) {
        row.set("detail", Json::text(say::msg("srv.consent.unnamedAgreement", "Filed under an agreement the TV does not name.")));
    } else {
        Json variables = Json::object();
        variables.set("names", Json::text(listText(peers)));
        row.set("detail", Json::text(say::msg(
            "srv.consent.sameAgreement", "Accepted under the same agreement as {names}.", variables)));
    }
    return row;
}

bool jsTruthy(const Json &value) {
    if (!value.valid()) return false;
    if (value.isBoolean()) return value.boolean();
    if (value.isNumber()) return value.number() != 0;
    if (value.isString()) return !value.string().empty();
    return true;
}

std::vector<std::string> stringItems(const Json &value) {
    std::vector<std::string> result;
    if (!value.isArray()) return result;
    for (const Json &item : value.items())
        if (item.isString()) result.push_back(item.string());
    return result;
}

std::set<std::string> acceptedDocuments(const Json &documents) {
    std::set<std::string> accepted;
    const Json list = documents["eulaList"];
    if (!list.isArray()) return accepted;
    for (const Json &entry : list.items()) {
        if (jsTruthy(entry["accepted"]) && entry["id"].isString())
            accepted.insert(entry["id"].string());
    }
    return accepted;
}

bool documentsSatisfied(const std::vector<std::string> &required,
                        const std::set<std::string> &accepted) {
    if (required.empty()) return false;
    for (const std::string &id : required)
        if (accepted.find(id) == accepted.end()) return false;
    return true;
}
} // namespace

Json privacyConsentGroups(const Json &licenseMapping) {
    bool foundAny = false;
    return readAgreementGroups(licenseMapping, foundAny);
}

Json privacyConsentReadProjection(const Json &flags, const Json &licenseMapping,
                                  const Json &documents) {
    if (!flags.isObject()) return Json();
    bool mapFound = false;
    const Json groups = readAgreementGroups(licenseMapping, mapFound);
    Json known = Json::array();
    Json other = Json::array();
    std::map<std::string, Json> byKey;

    const auto makeLabelled = [&](const std::string &key, const std::string &label,
                                  const std::string &detail, bool described) {
        Json row = Json::object();
        row.set("key", Json::text(key));
        row.set("label", Json::text(label));
        row.set("detail", Json::text(detail));
        row.set("enabled", Json::flag(flags[key].boolean()));
        row.set("settable", Json::flag(privacyConsentSettable(key, groups)));
        row.set("group", Json::text(consentGroup(key)));
        if (described) row.set("described", Json::flag(true));
        return row;
    };

    for (const ConsentLabel &definition : kConsentLabels) {
        const std::string key = definition.key;
        if (!flags[key].isBoolean()) continue;
        Json row = makeLabelled(key, say::msg(definition.labelKey, definition.label),
                                 say::msg(definition.detailKey, definition.detail), false);
        known.add(row);
        byKey[key] = row;
    }

    std::vector<std::string> unknownKeys;
    std::vector<std::string> describedKeys;
    for (const std::string &key : flags.keys()) {
        if (key.size() <= 7 || key.compare(key.size() - 7, 7, "Allowed") != 0 ||
            !flags[key].isBoolean() || hidden(key) || consentLabel(key))
            continue;
        bool validKey = true;
        for (size_t index = 0; index + 7 < key.size(); ++index) {
            const unsigned char character = static_cast<unsigned char>(key[index]);
            if (!((character >= 'a' && character <= 'z') ||
                  (character >= 'A' && character <= 'Z') ||
                  (character >= '0' && character <= '9') || character == '_')) {
                validKey = false;
                break;
            }
        }
        if (!validKey) continue;
        const bool enabled = flags[key].boolean();
        const Json mappedDocs = groups[key];
        const std::string name = consentName(key);
        if (!enabled && !mappedDocs.valid() && name.empty() && lockedDetail(key).empty())
            continue;

        const ConsentLabel *document = nullptr;
        std::string documentId;
        if (!locked(key)) {
            std::vector<std::string> mapped = readStrings(mappedDocs);
            std::sort(mapped.begin(), mapped.end());  // as the server sorts a flag's agreements
            for (const std::string &id : mapped) {
                document = documentLabel(id);
                if (document) {
                    documentId = id;
                    break;
                }
            }
        }
        if (document) {
            // A flag an agreement describes is a known one, as in the server.
            Json row = makeLabelled(key, say::msg(document->labelKey, document->label),
                                 say::msg(document->detailKey, document->detail), true);
            row.set("group", Json::text(documentGroup(documentId)));
            byKey[key] = row;
            describedKeys.push_back(key);
            continue;
        }
        Json row = describeUnlabelled(key, enabled, groups, mapFound);
        other.add(row);
        byKey[key] = row;
        unknownKeys.push_back(key);
    }
    std::sort(describedKeys.begin(), describedKeys.end());
    for (const std::string &key : describedKeys) known.add(byKey[key]);
    std::sort(unknownKeys.begin(), unknownKeys.end());
    if (!unknownKeys.empty()) {
        other = Json::array();
        for (const std::string &key : unknownKeys) other.add(byKey[key]);
    }

    Json result = Json::object();
    result.set("known", known);
    result.set("other", other);
    if (!documents["eulaList"].isArray()) {
        result.set("groups", groupRows());
        return result;
    }

    std::map<std::string, std::string> titles;
    for (const Json &entry : documents["eulaList"].items()) {
        if (entry["id"].isString() && entry["title"].isString() &&
            !entry["title"].string().empty())
            titles[entry["id"].string()] = entry["title"].string();
    }
    std::vector<std::string> rowKeys;
    for (const auto &entry : byKey) rowKeys.push_back(entry.first);
    for (const std::string &key : rowKeys) {
        Json row = byKey[key];
        const std::vector<std::string> required = readStrings(groups[key]);
        std::vector<std::string> agreementTitles;
        for (const std::string &id : required) {
            const auto found = titles.find(id);
            if (found != titles.end()) agreementTitles.push_back(found->second);
        }
        if (!agreementTitles.empty()) {
            Json agreements = Json::array();
            for (const std::string &title : agreementTitles)
                agreements.add(Json::text(title));
            row.set("agreements", agreements);
            if (!consentLabel(key) && !row["described"].boolean()) {
                Json variables = Json::object();
                variables.set("agreements", Json::text(listText(agreementTitles)));
                row.set("detail", Json::text(say::msg(
                    "srv.consent.acceptedUnder", "Accepted under {agreements}.", variables)));
            }
        }
        if (row["settable"].boolean() && row["enabled"].boolean() &&
            documents["eulaList"].isArray()) {
            const PrivacyConsentPlan plan = planPrivacyConsent(
                key, false, flags, documents, groups);
            std::vector<std::string> peers;
            for (const std::string &changed : plan.flags.keys()) {
                (void)changed;
            }
            for (const std::string &candidate : groups.keys()) {
                if (candidate == key || !plan.flags.has(candidate) ||
                    plan.flags[candidate].boolean() == flags[candidate].boolean())
                    continue;
                const auto peer = byKey.find(candidate);
                if (peer == byKey.end()) continue;
                const std::string label = peer->second["label"].string(candidate);
                peers.push_back(label);
            }
            if (!peers.empty()) {
                Json values = Json::array();
                for (const std::string &peer : peers) values.add(Json::text(peer));
                row.set("sharesWith", values);
            }
        }
        byKey[key] = row;
    }

    // Rebuild arrays after enrichment so their object copies include the new
    // agreement and peer details.
    known = Json::array();
    for (const ConsentLabel &definition : kConsentLabels) {
        const auto found = byKey.find(definition.key);
        if (found != byKey.end()) known.add(found->second);
    }
    for (const std::string &key : describedKeys) known.add(byKey[key]);
    other = Json::array();
    for (const std::string &key : unknownKeys) other.add(byKey[key]);
    result.set("known", known);
    result.set("other", other);
    result.set("groups", groupRows());
    return result;
}

Json privacySimpleSummary(const Json &projection) {
    const Json consent = projection["consent"];
    std::vector<Json> flags;
    for (const char *listName : {"known", "other"}) {
        for (const Json &row : consent[listName].items()) flags.push_back(row);
    }
    const bool writable = !(projection["consentWritable"].isBoolean() &&
                            !projection["consentWritable"].boolean());
    const auto onIn = [&flags, writable](const std::string &group) {
        Json items = Json::array();
        for (const Json &flag : flags) {
            const std::string key = flag["key"].string();
            if (flag["group"].string() != group || !flag["enabled"].boolean() ||
                !flag["settable"].boolean() || !writable ||
                key == "voiceAllowed" || key == "voice2Allowed")
                continue;
            Json item = Json::object();
            item.set("label", flag["label"].isString()
                ? flag["label"] : Json::text(key));
            item.set("action", Json::text("consent"));
            Json value = Json::object();
            value.set("key", Json::text(key));
            value.set("enabled", Json::flag(false));
            item.set("value", value);
            items.add(item);
        }
        return items;
    };

    Json watching = onIn("watching");
    const Json acr = projection["acr"];
    if (acr.isObject() && writable &&
        (acr["active"].boolean() ||
         (acr["livePlus"].isBoolean() && acr["livePlus"].boolean()))) {
        Json item = Json::object();
        item.set("label", Json::text(acr["active"].boolean()
            ? "Content recognition is running" : "Live Plus is on"));
        item.set("action", Json::text("acr"));
        item.set("value", Json::flag(false));
        watching.add(item);
    }

    Json ads = onIn("advertising");
    const Json ad = projection["advertisingId"];
    if ((ad["available"].boolean() || ad["lmtKnown"].boolean()) &&
        !ad["limitTracking"].boolean() && writable) {
        Json item = Json::object();
        item.set("label", Json::text(say::msg("srv.privacy.item.lmt", "Limit ad tracking is off")));
        item.set("action", Json::text("limitAdTracking"));
        item.set("value", Json::flag(true));
        ads.add(item);
    }
    const std::string mode = projection["adblock"]["mode"].string("off");
    if (mode == "off") {
        Json item = Json::object();
        item.set("label", Json::text(say::msg("srv.privacy.item.adblock", "LG’s ad and tracking servers can be reached")));
        item.set("action", Json::text("setAdBlock"));
        item.set("value", Json::text("ads"));
        ads.add(item);
    }

    Json reports = onIn("analytics");
    for (const Json &daemon : projection["daemons"].items()) {
        if (daemon["onDemand"].boolean() || !daemon["stoppable"].boolean() ||
            !daemon["running"].boolean())
            continue;
        Json item = Json::object();
        Json variables = Json::object();
        variables.set("name", daemon["label"]);
        item.set("label", Json::text(say::msg(
            "srv.privacy.item.running", "{name} is running", variables)));
        item.set("service", daemon["name"]);
        reports.add(item);
    }

    Json onScreen = Json::array();
    for (const Json &setting : projection["lgSettings"].items()) {
        if (setting["section"].string() != "promotions" ||
            !setting["on"].boolean() || !writable)
            continue;
        Json item = Json::object();
        item.set("label", setting["title"]);
        item.set("action", Json::text("lgSetting"));
        Json value = Json::object();
        value.set("id", setting["id"]);
        value.set("on", Json::flag(false));
        item.set("value", value);
        onScreen.add(item);
    }

    Json areas = Json::array();
    // id, then each text beside its say::msg() key.
    const char *definitions[][5] = {
        {"watching", "srv.privacy.area.watching", "Screen recognition",
         "srv.privacy.area.watching.detail", "LG identifying what you watch, to target ads at you."},
        {"ads", "srv.privacy.area.ads", "Ad tracking",
         "srv.privacy.area.ads.detail", "Advertisers tracking the TV across apps, to target ads at you."},
        {"reports", "srv.privacy.area.reports", "Usage reports",
         "srv.privacy.area.reports.detail", "Usage and diagnostic reports sent to LG, and data passed to other companies."},
        {"onScreen", "srv.privacy.area.onScreen", "LG ads on screen",
         "srv.privacy.area.onScreen.detail", "Adverts, sponsored tiles and tips that LG shows on the TV."}
    };
    const Json areaItems[] = {watching, ads, reports, onScreen};
    size_t total = 0;
    for (size_t index = 0; index < 4; ++index) {
        Json area = Json::object();
        area.set("id", Json::text(definitions[index][0]));
        area.set("name", Json::text(say::msg(definitions[index][1], definitions[index][2])));
        area.set("detail", Json::text(say::msg(definitions[index][3], definitions[index][4])));
        area.set("items", areaItems[index]);
        total += areaItems[index].size();
        areas.add(area);
    }

    Json kept = Json::array();
    for (const Json &flag : flags) {
        const std::string key = flag["key"].string();
        if (!flag["enabled"].boolean() ||
            (key != "voiceAllowed" && key != "voice2Allowed" &&
             flag["group"].string() != "services"))
            continue;
        kept.add(flag["label"].isString() ? flag["label"] : Json::text(key));
    }

    Json summary = Json::object();
    summary.set("total", Json::numeric(static_cast<double>(total)));
    summary.set("areas", areas);
    summary.set("kept", kept);
    return summary;
}

bool privacyConsentSettable(const std::string &key, const Json &groups) {
    if (key.empty() || locked(key) || hidden(key)) return false;
    for (const char *known : kNamedSettable)
        if (key == known) return true;
    return groups.isObject() && groups.has(key);
}

PrivacyConsentPlan planPrivacyConsent(const std::string &key, bool enabled,
                                     const Json &flags, const Json &documents,
                                     const Json &groups) {
    PrivacyConsentPlan result;
    if (key.empty()) {
        result.error = "no consent flag named";
        return result;
    }
    if (!privacyConsentSettable(key, groups)) {
        result.error = key + " is not changeable from here";
        return result;
    }
    if (!flags.isObject() || !documents.isObject() || !documents["eulaList"].isArray()) {
        result.error = "could not read the consent records";
        return result;
    }
    if (!flags.has(key)) {
        result.error = "no such consent flag: " + key;
        return result;
    }

    std::set<std::string> accepted = acceptedDocuments(documents);
    const std::vector<std::string> required = stringItems(groups[key]);
    if (enabled) {
        accepted.insert(required.begin(), required.end());
    } else {
        std::set<std::string> protectedDocuments;
        for (const std::string &candidate : groups.keys()) {
            if (!locked(candidate) || !jsTruthy(flags[candidate])) continue;
            const std::vector<std::string> protectedIds = stringItems(groups[candidate]);
            protectedDocuments.insert(protectedIds.begin(), protectedIds.end());
        }
        for (const std::string &id : required)
            if (protectedDocuments.find(id) == protectedDocuments.end())
                accepted.erase(id);
    }

    Json nextFlags = Json::parse(flags.dump());
    std::vector<std::string> changed;
    for (const std::string &candidate : groups.keys()) {
        if (!nextFlags.has(candidate) || locked(candidate)) continue;
        const std::vector<std::string> candidateDocs = stringItems(groups[candidate]);
        if (jsTruthy(nextFlags[candidate]) &&
            !documentsSatisfied(candidateDocs, accepted)) {
            nextFlags.set(candidate, Json::flag(false));
            changed.push_back(candidate);
        }
    }
    const Json oldTarget = nextFlags[key];
    const bool targetAlreadyMatches = oldTarget.isBoolean() && oldTarget.boolean() == enabled;
    if (!targetAlreadyMatches) {
        nextFlags.set(key, Json::flag(enabled));
        if (std::find(changed.begin(), changed.end(), key) == changed.end())
            changed.push_back(key);
    }

    Json nextDocuments = Json::parse(documents.dump());
    Json nextList = Json::array();
    for (const Json &entry : documents["eulaList"].items()) {
        Json updated = Json::parse(entry.dump());
        const Json id = entry["id"];
        if (id.isString())
            updated.set("accepted", Json::flag(accepted.find(id.string()) != accepted.end()));
        nextList.add(updated);
    }
    nextDocuments.set("eulaList", nextList);

    result.valid = true;
    result.changed = !changed.empty();
    result.alsoChanged = changed.empty() ? 0 : static_cast<unsigned>(changed.size() - 1);
    result.flags = nextFlags;
    result.documents = nextDocuments;
    return result;
}

} // namespace service
} // namespace nativetv
