#include "scene/depth_policy.h"

#include "util/log.h"

#include <cstdlib>

namespace bro::scene {

bool gReversedZ = false;

bool initDepthPolicy() {
    // Probed once per process. The GL context is created once and never
    // recreated, and glClipControl is context state, so re-probing would at
    // best be a no-op and at worst flip the convention out from under
    // already-compiled shaders.
    static bool probed = false;
    if (probed) return gReversedZ;
    probed = true;

    // Escape hatch: forces the conventional path on hardware that supports
    // clip control. Without it the fallback is untestable anywhere it
    // matters — every desktop driver advertises the extension, so the branch
    // that machines lacking it would take would never run here.
    if (const char* off = std::getenv("BRO_DISABLE_REVERSED_Z");
        off && *off && off[0] != '0') {
        LOG_INFO("scene: reversed-Z disabled by BRO_DISABLE_REVERSED_Z");
        gReversedZ = false;
        return false;
    }

    // Vulkan natively uses [0, 1] depth; reversed-Z is enabled by default.
    gReversedZ = true;
    return true;
}

}  // namespace bro::scene
