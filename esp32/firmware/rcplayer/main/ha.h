// Home Assistant client: polls a few entities over HA's REST API and publishes them to the
// documents as named variables (docs/RC_SYSTEM_VARIABLES.md):
//   ha.online                 1 while the last poll worked, else 0
//   ha.<entity_id>            the entity's state ("heat", "on", "23.5", ...)
//   ha.<entity_id>.<attr>     each number or string attribute (null -> "")
// The server, the long-lived token and the entity list are saved in NVS from the console
// ("ha url ...", "ha token ...", "ha entities ..."); the token is never printed. With no
// entities it doesn't poll (Home Assistant pushes them, POST /named) but still makes calls.
#pragma once
#include <string>

namespace ha {

void start();                                  // background task; polls while on Wi-Fi
bool console(const std::string& args);         // the "ha ..." console command
void pollNow();
// Ask Home Assistant to set a climate entity's set point: attr "temperature", or
// "target_temp_low" / "target_temp_high" (a range: give both). Returns at once; what one entity
// is asked within 150 ms goes as one climate.set_temperature call, then the entity is read
// back and published, and done(entity, attr) runs for each attribute asked, whether or not it
// worked. HA task.
void setClimate(const std::string& entity, const std::string& attr, float value);
void onSetDone(void (*done)(const std::string& entity, const std::string& attr));

}  // namespace ha
