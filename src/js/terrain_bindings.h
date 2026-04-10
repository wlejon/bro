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

} // namespace bro::js
