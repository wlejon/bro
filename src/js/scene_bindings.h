#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::scene { class SceneGraph; }
namespace bro::physics { class PhysicsWorld; }

namespace bro::js {

class SceneBindings {
public:
    /// Install the Scene class constructors/prototypes into the JS context.
    static void install(JSContext* ctx);

    /// Create and wrap a SceneGraph JS object bound to the given graph.
    static JSValue wrapSceneGraph(JSContext* ctx, scene::SceneGraph* graph);

    /// Clean up class IDs and prototypes.
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
