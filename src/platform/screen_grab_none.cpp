// screen_grab.h where bro has no desktop backend: a Linux build without
// browl (BRO_WITH_WL off). Under `bro --drm` the engine reads its own scanout.
#include "platform/screen_grab.h"

namespace bro::platform::screen {

namespace {
constexpr const char* kWhy = "this build has no desktop capture backend (Wayland support, BRO_WITH_WL, is off)";
}

std::vector<Display> displays(std::string* why) {
    if (why) *why = kWhy;
    return {};
}

Status grab(int, int, int, int, Grab&, std::string& why, bool) {
    why = kWhy;
    return Status::Unsupported;
}

}  // namespace bro::platform::screen
