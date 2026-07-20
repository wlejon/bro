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

    // GLAD resolves glClipControl through the ARB_clip_control extension
    // loader, so in a 3.3 core context the pointer is non-null exactly when
    // the driver advertises the extension. Check both: the flag tells us the
    // driver claims it, the pointer tells us we actually got an entry point.
    if (!GLAD_GL_ARB_clip_control || glad_glClipControl == nullptr) {
        LOG_WARN("scene: GL_ARB_clip_control unavailable; keeping conventional "
                 "depth. Distant geometry will z-fight at large view ranges.");
        gReversedZ = false;
        return false;
    }

    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    if (glGetError() != GL_NO_ERROR) {
        LOG_WARN("scene: glClipControl rejected; keeping conventional depth.");
        gReversedZ = false;
        return false;
    }

    gReversedZ = true;
    LOG_INFO("scene: reversed-Z depth enabled (32F depth buffer)");
    return true;
}

}  // namespace bro::scene
