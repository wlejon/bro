#pragma once

#include <qjsbind/qjsbind.h>

#include <string>

namespace bro::scene { class SceneGraph; }
namespace bro::util { class AssetMounts; }

namespace bro::js {

class TileBindings {
public:
    /// Install the TileWorld class prototype into the JS context.
    static void install(JSContext* ctx);

    /// App-relative path resolution context for atlas images (set per app load).
    static void setAppContext(const std::string& basePath,
                              const util::AssetMounts* mounts);

    /// Clean up live TileWorlds before scene graphs are destroyed.
    static void cleanup(JSContext* ctx);
};

/// Create a TileWorld JS object. Called by scene_bindings when
/// scene.createTileWorld(opts) is invoked.
JSValue createTileWorldJS(JSContext* ctx, scene::SceneGraph* graph, JSValueConst opts);

} // namespace bro::js
