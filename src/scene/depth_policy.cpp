#include "scene/depth_policy.h"

#include "util/log.h"

#include <cstdlib>

namespace bro::scene {

bool reversedZ() {
    // Read once. BRO_DISABLE_REVERSED_Z is the escape hatch that keeps the
    // conventional path exercised: nothing in Vulkan requires it, so without
    // the switch the branch would never run anywhere.
    static const bool reversed = [] {
        const char* off = std::getenv("BRO_DISABLE_REVERSED_Z");
        if (off && *off && off[0] != '0') {
            LOG_INFO("scene: reversed-Z disabled by BRO_DISABLE_REVERSED_Z");
            return false;
        }
        return true;
    }();
    return reversed;
}

}  // namespace bro::scene
