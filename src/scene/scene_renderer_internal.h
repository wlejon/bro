#pragma once

// Shared GL helpers for the SceneRenderer translation units. SceneRenderer's
// implementation is split across scene_renderer*.cpp (mesh, instanced, shadow,
// post-FX, environment, overlays, lighting, core); these small helpers are
// used by all of them, so they live here as inline functions rather than a
// file-local static in any one unit.

#include "webgl/webgl_types.h"

#include <cassert>
#include <cstring>
#include <string>

#include "util/log.h"
#include "scene/depth_policy.h"

namespace bro::scene {

}  // namespace bro::scene
