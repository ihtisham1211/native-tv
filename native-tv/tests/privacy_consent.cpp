#include "service/privacy_consent.hpp"
#include <stdexcept>
#include <string>

using namespace nativetv;
using namespace nativetv::service;

namespace {
void check(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

Json groupsFrom(const std::string &text) {
    return privacyConsentGroups(Json::parse(text));
}

void testMappingAndSettableRules() {
    const Json groups = groupsFrom(
        R"({"eulaMappingList":{"eulaInfo":[{"settingKey":"acrOnAllowed","mandatory":["acr-doc"],"generalSelectAll":["terms-doc"]},{"settingKey":"thirdPartySharingAllowed","generalSelectAll":["dpa"]}]}})"
    );
    check(groups["acrOnAllowed"].size() == 1 &&
              groups["acrOnAllowed"][0].string() == "acr-doc" &&
              groups["thirdPartySharingAllowed"][0].string() == "dpa",
          "mandatory agreement ids take precedence and generalSelectAll is the fallback");
    check(privacyConsentSettable("acrAllowed", groups),
          "documented consent keys remain writable without a firmware map");
    check(privacyConsentSettable("acrOnAllowed", groups),
          "firmware-mapped consent keys are writable");
    check(!privacyConsentSettable("generalTermsAllowed", groups) &&
              !privacyConsentSettable("chpAllowed", groups) &&
              !privacyConsentSettable("unmappedAllowed", groups),
          "locked, hidden and unknown flags stay unwritable");
}

void testReadProjectionMatchesConsentDashboardContract() {
    const Json mapping = Json::parse(
        R"({"eulaMappingList":{"eulaInfo":[{"settingKey":"acrAllowed","mandatory":["s_acr"]},{"settingKey":"acrAdAllowed","mandatory":["s_acr"]},{"settingKey":"additional1Allowed","mandatory":["S_DPA"]},{"settingKey":"generalTermsAllowed","mandatory":["terms"]}]}})"
    );
    const Json flags = Json::parse(
        R"({"unknown2Allowed":false,"acrAdAllowed":true,"acrAllowed":true,"chpAllowed":true,"additional1Allowed":true,"generalTermsAllowed":true,"emptyAllowed":false,"voiceAllowed":true})"
    );
    const Json documents = Json::parse(
        R"({"eulaList":[{"id":"s_acr","title":"Screen recognition terms","accepted":true},{"id":"S_DPA","title":"Data partners","accepted":true},{"id":"terms","title":"Terms","accepted":true}]})"
    );
    const Json groups = privacyConsentGroups(mapping);
    const PrivacyConsentPlan shared = planPrivacyConsent(
        "acrAllowed", false, flags, documents, groups);
    check(shared.valid && !shared.flags["acrAdAllowed"].boolean(),
          "shared-consent planner identifies a peer flag to disable");
    check(flags["acrAllowed"].boolean() && flags["acrAdAllowed"].boolean(),
          "consent planning leaves the caller's TV snapshot unchanged");
    check(documents["eulaList"][0]["accepted"].boolean(),
          "consent planning leaves the caller's agreement records unchanged");
    const PrivacyConsentPlan voice = planPrivacyConsent(
        "voiceAllowed", false, flags, documents, groups);
    check(voice.valid && voice.flags["acrAllowed"].boolean() &&
              voice.flags["acrAdAllowed"].boolean() &&
              voice.flags["additional1Allowed"].boolean(),
          "withdrawing an unmapped consent does not change other mapped flags");
    const Json projection = privacyConsentReadProjection(flags, mapping, documents);
    check(projection.isObject() && projection["known"].size() == 4,
          "named rows follow the stable consent label order, then the agreement-described ones");
    check(projection["known"][0]["key"].string() == "acrAllowed" &&
              projection["known"][1]["key"].string() == "acrAdAllowed" &&
              projection["known"][2]["key"].string() == "voiceAllowed",
          "known flags are ordered by the documented catalog");
    check(projection["known"][0]["agreements"][0].string() == "Screen recognition terms",
          "read projection includes agreement titles");
    check(projection["known"][0]["sharesWith"].size() == 1 &&
              projection["known"][0]["sharesWith"][0].string() == "Ads based on what you watch",
          "read projection includes shared-consent effects");
    // A flag an agreement describes is a known one (and takes the agreement's group),
    // as on a TV; only the locked one stays in `other`.
    check(projection["known"][3]["key"].string() == "additional1Allowed" &&
              projection["known"][3]["label"].string() == "Sharing viewing data with data partners" &&
              projection["known"][3]["group"].string() == "analytics" &&
              projection["known"][3]["described"].boolean(),
          "an agreement-described flag is listed as known, with the agreement's group");
    check(projection["other"].size() == 1 &&
              projection["other"][0]["key"].string() == "generalTermsAllowed" &&
              projection["other"][0]["group"].string() == "platform",
          "locked flags keep their labels, order and groups");
    check(projection["groups"].size() == 6 &&
              projection["groups"][5][0].string() == "platform",
          "the projection includes the six consent groups used by the dashboard");
}

void testSimpleSummaryMatchesPrivacyActions() {
    const Json projection = Json::parse(
        R"({"consentWritable":true,"consent":{"known":[{"key":"acrAllowed","label":"Screen recognition","enabled":true,"settable":true,"group":"watching"},{"key":"voiceAllowed","label":"Voice","enabled":true,"settable":true,"group":"watching"},{"key":"additionalDataAllowed","enabled":true,"settable":true,"group":"analytics"}],"other":[{"key":"networkAllowed","label":"Network use","enabled":true,"settable":false,"group":"platform"},{"key":"familyAllowed","label":"Family service","enabled":true,"settable":false,"group":"services"}]},"acr":{"active":false,"livePlus":true},"advertisingId":{"available":true,"lmtKnown":true,"limitTracking":false},"adblock":{"mode":"off"},"daemons":[{"name":"uploadd","label":"Diagnostics uploader","onDemand":false,"stoppable":true,"running":true},{"name":"admanager","label":"Advertising service","onDemand":true,"stoppable":false,"running":true}],"lgSettings":[{"section":"promotions","id":"screenSaverAd","title":"Screen saver ads","on":true}]})"
    );
    const Json summary = privacySimpleSummary(projection);
    check(summary["total"].number() == 7 && summary["areas"].size() == 4,
          "summary counts actionable items across four privacy areas");
    check(summary["areas"][0]["items"].size() == 2 &&
              summary["areas"][0]["items"][1]["action"].string() == "acr",
          "watching area keeps the consent switch and Live Plus action");
    check(summary["areas"][1]["items"].size() == 2 &&
              summary["areas"][1]["items"][1]["action"].string() == "setAdBlock",
          "ad area includes the LMT and ad-block actions");
    check(summary["areas"][2]["items"].size() == 2 &&
              summary["areas"][2]["items"][1]["service"].string() == "uploadd",
          "reports include a running stoppable service but skip on-demand services");
    check(summary["areas"][3]["items"].size() == 1 &&
              summary["areas"][3]["items"][0]["value"]["id"].string() == "screenSaverAd",
          "on-screen area includes enabled LG promotion settings");
    check(summary["kept"].size() == 2 && summary["kept"][0].string() == "Voice" &&
              summary["kept"][1].string() == "Family service",
          "summary calls out voice and LG services that remain enabled");
}

void testWithdrawalUpdatesSharedFlagsAndDocuments() {
    const Json groups = groupsFrom(
        R"({"eulaMappingList":{"eulaInfo":[{"settingKey":"acrAllowed","mandatory":["acr-doc"]},{"settingKey":"acrAdAllowed","mandatory":["acr-doc"]}]}})"
    );
    const Json flags = Json::parse(
        R"({"acrAllowed":true,"acrAdAllowed":true,"unrelated":true})");
    const Json docs = Json::parse(
        R"({"region":"GB","eulaList":[{"id":"acr-doc","accepted":true},{"id":"other-doc","accepted":true}]})");
    const PrivacyConsentPlan plan = planPrivacyConsent(
        "acrAllowed", false, flags, docs, groups);
    check(plan.valid && plan.changed && plan.alsoChanged == 1,
          "withdrawal counts the other flag that shares its agreement");
    check(!plan.flags["acrAllowed"].boolean() &&
              !plan.flags["acrAdAllowed"].boolean() &&
              plan.flags["unrelated"].boolean(),
          "withdrawal clears affected flags and preserves unrelated ones");
    check(!plan.documents["eulaList"][0]["accepted"].boolean() &&
              plan.documents["eulaList"][1]["accepted"].boolean() &&
              plan.documents["region"].string() == "GB",
          "withdrawal changes only related agreements and preserves the envelope");
}

void testLockedAgreementProtectionAndEnable() {
    const Json groups = groupsFrom(
        R"({"eulaMappingList":{"eulaInfo":[{"settingKey":"acrAllowed","mandatory":["terms-doc"]},{"settingKey":"acrAdAllowed","mandatory":["terms-doc"]},{"settingKey":"generalTermsAllowed","mandatory":["terms-doc"]},{"settingKey":"thirdPartySharingAllowed","mandatory":["dpa"]}]}})"
    );
    const Json flags = Json::parse(
        R"({"acrAllowed":true,"acrAdAllowed":true,"generalTermsAllowed":true,"thirdPartySharingAllowed":false})");
    const Json docs = Json::parse(
        R"({"eulaList":[{"id":"terms-doc","accepted":true},{"id":"dpa","accepted":false}]})");
    const PrivacyConsentPlan withdrawal = planPrivacyConsent(
        "acrAllowed", false, flags, docs, groups);
    check(withdrawal.valid && withdrawal.documents["eulaList"][0]["accepted"].boolean(),
          "a locked, accepted agreement keeps its document accepted");
    check(!withdrawal.flags["acrAllowed"].boolean() &&
              withdrawal.flags["acrAdAllowed"].boolean(),
          "the requested flag can be disabled while the locked peer stays enabled");

    const PrivacyConsentPlan enable = planPrivacyConsent(
        "thirdPartySharingAllowed", true, flags, docs, groups);
    check(enable.valid && enable.changed && enable.flags["thirdPartySharingAllowed"].boolean(),
          "enabling a consent turns on its flag");
    check(enable.documents["eulaList"][1]["accepted"].boolean(),
          "enabling a consent accepts its mapped agreement");
}

void testNoOpAndInvalidRequests() {
    const Json groups = groupsFrom(R"({"eulaMappingList":{"eulaInfo":[]}})");
    const Json flags = Json::parse(R"({"acrAllowed":false})");
    const Json docs = Json::parse(R"({"eulaList":[]})");
    const PrivacyConsentPlan noOp = planPrivacyConsent(
        "acrAllowed", false, flags, docs, groups);
    check(noOp.valid && !noOp.changed,
          "an already disabled consent produces a no-write result");
    const PrivacyConsentPlan hidden = planPrivacyConsent(
        "chpAllowed", false, Json::parse(R"({"chpAllowed":true})"), docs, groups);
    check(!hidden.valid && hidden.error == "chpAllowed is not changeable from here",
          "the LG Channels flag is rejected without a Luna write");
    const PrivacyConsentPlan missing = planPrivacyConsent(
        "", false, flags, docs, groups);
    check(!missing.valid && missing.error == "no consent flag named",
          "empty consent names are rejected");
}
} // namespace

int main() {
    testMappingAndSettableRules();
    testReadProjectionMatchesConsentDashboardContract();
    testSimpleSummaryMatchesPrivacyActions();
    testWithdrawalUpdatesSharedFlagsAndDocuments();
    testLockedAgreementProtectionAndEnable();
    testNoOpAndInvalidRequests();
    return 0;
}
