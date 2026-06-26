#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::scene { class SceneGraph; }

namespace bro::js {

class TileBindings {
public:
    /// Install the TileWorld class prototype into the JS context.
    static void install(JSContext* ctx);

    /// Clean up live TileWorlds before scene graphs are destroyed.
    static void cleanup(JSContext* ctx);
};

/// Create a TileWorld JS object. Called by scene_bindings when
/// scene.createTileWorld(opts) is invoked.
JSValue createTileWorldJS(JSContext* ctx, scene::SceneGraph* graph, JSValueConst opts);

} // namespace bro::js
