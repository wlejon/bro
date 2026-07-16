#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::scene { class SceneGraph; }

namespace bro::js {

class TerrainBindings {
public:
    /// Install the Terrain class prototype into the JS context.
    static void install(JSContext* ctx);

    /// Clean up prototypes.
    static void cleanup(JSContext* ctx);
};

/// Create a Terrain JS object. Called by scene_bindings when
/// scene.createTerrain(opts) is invoked.
JSValue createTerrainJS(JSContext* ctx, scene::SceneGraph* graph, JSValueConst opts);

/// Opaque handle to the TerrainWrapper behind a JS terrain object (nullptr
/// if the value is not a Terrain). Used by the AI ground-follow glue, which
/// probes heights every frame without re-entering JS.
void* terrainHandleFromJS(JSContext* ctx, JSValueConst v);

/// Height probe for AI ground-follow: raycast straight down from
/// (x, rayStartY, z) through the terrain behind `handle` (a value from
/// terrainHandleFromJS). Returns false when the terrain has been destroyed
/// or nothing was hit within rayLength. Safe against stale handles — checks
/// the live-instance registry before dereferencing.
bool terrainSampleHeight(void* handle, float x, float z,
                         float rayStartY, float rayLength, float& outY);

} // namespace bro::js
