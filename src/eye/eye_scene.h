#pragma once

#include "eye/eye_types.h"

namespace bro::scene { class SceneGraph; }

namespace bro::eye {

/// Audits a live 3D SceneGraph using rendered pixels, reversed-Z depth buffer,
/// camera matrices, mesh geometry, and BVH spatial queries.
EyeSceneResult auditScene(scene::SceneGraph& graph, const EyeSceneOptions& opts = {});

}  // namespace bro::eye
