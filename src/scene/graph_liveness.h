#pragma once

// The SceneGraph liveness token, on its own so that a header which must not
// pull in all of scene_graph.h can still hold one.
//
// The engine owns every SceneGraph, keyed to its canvas element, and destroys
// it when that element leaves the DOM (Engine::pruneDetachedSceneGraphs) or at
// teardown. Anything that outlives the graph — a JS wrapper, or a scene-owned
// helper object whose handle JS is still holding — must not keep a SceneGraph&
// or SceneGraph*: it re-resolves through a weak_ptr to this token on every use.
// A failed lock(), or a null `graph`, means the graph is gone and the caller
// no-ops instead of touching freed memory. ~SceneGraph nulls `graph` first
// thing, before any of its own state is destroyed.
//
// SceneGraph::LivenessToken is an alias for this type, so the name the JS
// binding layer already uses keeps working (see scene_bindings_internal.h).

namespace bro::scene {

class SceneGraph;

struct GraphLivenessToken {
    SceneGraph* graph = nullptr;
};

} // namespace bro::scene
