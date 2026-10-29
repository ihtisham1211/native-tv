#pragma once

#include "core/json.hpp"
#include <string>
#include <vector>

namespace nativetv {
namespace service {

// The pure pieces of the /api/stats response (legacy lib/telemetry.js): what a
// Luna answer says, and nothing else. The collector that asks the TV and keeps
// the caches is separate, so each of these runs against a fixed answer.

// The format and HDR metadata of the connected sink in videooutput's getStatus,
// or null with none connected or no videoInfo (standby).
Json statsSignalFormat(const Json &videoOutputReply);

// How the volume can be changed with the sound going where it is now: "level",
// "steps" or "none". vs is the newer audio service's volumeStatus, sound the
// older getSoundOut answer; either may be null.
std::string statsVolumeControl(const Json &vs, const Json &sound);
// Where the sound goes, as the audio service names it, or "" for neither.
std::string statsSoundRoute(const Json &vs, const Json &sound);
// The output the sound is going to, as the dashboard shows it.
std::string statsAudioOutput(const Json &vs, const Json &sound);

// The sound outputs the TV offers, in LG's order: values is the settings
// service's arrayExt for soundOutput, cfg configd's answer for the model's
// features, current the output in use (always kept). Empty where none listed.
std::vector<std::string> statsOfferedSoundOutputs(const Json &values, const Json &cfg,
                                                  const std::string &current);

// "HH:MM" from LG's separate hour and minute strings ("1" and "0" is 01:00).
std::string statsClockTime(const Json &hour, const Json &minute);

// LG's On Timer and Off Timer from the 'time' settings, as the dashboard
// shows them: {on?, off?}, or null where neither exists.
Json statsTimersFromSettings(const Json &settings);

// The apps shown in the dashboard: not hidden, not a container, once each,
// by title. titles gets every app's title by id, hidden ones included.
Json statsAppList(const Json &raw, Json &titles);

} // namespace service
} // namespace nativetv
