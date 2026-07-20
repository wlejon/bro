#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::scene { class SceneGraph; }

namespace bro::js {

class ClipmapBindings {
public:
    /// Install the ClipmapTerrain class prototype into the JS context.
    static void install(JSContext* ctx);

    /// Destroy every live ClipmapTerrain before its SceneGraph goes away.
    static void cleanup(JSContext* ctx);
};

/// Create a ClipmapTerrain JS object. Called by scene_bindings when
/// scene.createClipmapTerrain(opts) is invoked.
JSValue createClipmapTerrainJS(JSContext* ctx, scene::SceneGraph* graph,
                               JSValueConst opts);

} // namespace bro::js
