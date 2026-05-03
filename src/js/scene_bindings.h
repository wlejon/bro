#pragma once

extern "C" {
#include "quickjs.h"
}

#include <qjsbind/qjsbind.h>

#include <string>

namespace bro::scene { class SceneGraph; }
namespace bro::physics { class PhysicsWorld; }
namespace bro::util { class AssetMounts; }

namespace bro::js {

class SceneBindings {
public:
    /// Install the Scene class constructors/prototypes into the JS context.
    static void install(JSContext* ctx);

    /// Set the app base path and asset mounts used to resolve relative file
    /// paths passed to scene APIs (e.g. setEnvironment hdr). Called per app
    /// load after the manifest is resolved.
    static void setAppContext(const std::string& basePath,
                              const util::AssetMounts* mounts);

    /// Create and wrap a SceneGraph JS object bound to the given graph.
    static JSValue wrapSceneGraph(JSContext* ctx, scene::SceneGraph* graph);

    /// Clean up class IDs and prototypes.
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
