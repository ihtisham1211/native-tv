#include "service/home_guide.hpp"
#include <cassert>
#include <iostream>

using namespace nativetv;
using namespace nativetv::service;

int main() {
    const int64_t now = 1000000000000LL;
    Json reply = Json::object();
    reply.set("returnValue", Json::flag(true));
    reply.set("programName", Json::text("\u00a0Evening News\u2003"));
    reply.set("startTime", Json::text("2001,9,9,01,00,00"));
    reply.set("endTime", Json::text("2001,9,9,02,00,00"));
    Json programme = homeCurrentProgramme(reply, now);
    assert(programme.isObject());
    assert(programme["title"].string() == "Evening News");
    assert(programme["start"].number() < now);
    assert(programme["end"].number() > now);

    reply.set("startTime", Json::text("2001,9,9,03,00,00"));
    assert(!homeCurrentProgramme(reply, now).valid());
    reply.set("startTime", Json::text("2001,9,9,01,00,00"));
    reply.set("endTime", Json::text("2001,9,9,01,00,00"));
    assert(!homeCurrentProgramme(reply, now).valid());
    reply.set("endTime", Json::text("2001,9,9,02,00,00"));
    reply.set("programName", Json::text(" \u00a0 "));
    assert(!homeCurrentProgramme(reply, now).valid());
    reply.set("programName", Json::text("News"));
    reply.set("returnValue", Json::flag(false));
    assert(!homeCurrentProgramme(reply, now).valid());

    std::cout << "Native Home guide projection passed.\n";
    return 0;
}
