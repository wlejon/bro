// app_schedule.h where bro has no scheduler backend: a Linux build without
// brodbus (the desktop substrate off), which cannot reach systemd.
#include "platform/app_schedule.h"

namespace bro::platform::schedule {

namespace {
constexpr const char* kWhy =
    "this build cannot schedule launches (systemd is reached over D-Bus, which needs the desktop substrate, BRO_WITH_SEAT)";
}

std::string backend(std::string* why) {
    if (why) *why = kWhy;
    return {};
}

bool add(const Launch&, std::string& why) {
    why = kWhy;
    return false;
}

bool remove(const std::string&, std::string*) { return true; }

bool exists(const std::string&) { return false; }

}  // namespace bro::platform::schedule
