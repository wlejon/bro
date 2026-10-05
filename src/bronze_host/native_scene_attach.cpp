// native_scene_attach.cpp — moving a SceneGraph between canvases without
// rebuilding it (docs/scene-api.js, "Moving a scene to another canvas"):
// SceneGraph.attachTo(canvas), detach(), attached, keepAlive. The binding
// itself is the engine's (SceneGraph::rebindCanvas, installed by
// Engine::createSceneContext); this file validates the canvas and keeps the
// canvas side (getContext('scene') answering the moved scene) in step.

#include "bronze_host/native_scene_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_realm_scope.h"
#include "bronze_host/host_rooted.h"
#include "bronze_host/host_element.h"
#include "bronze_host/host_runtime.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "util/string_utils.h"

#include <string>

namespace bro::bronze_host {

std::string adoptSceneContext(dom::Element* el, Value canvasValue, Value scene);

namespace {

HostSceneGraphCell* sceneCellOfValue(Value v) {
    if (!ev::isObject(v)) return nullptr;
    return graphCellOf(ev::handleData(v));
}

}  // namespace

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;
using namespace bro;

// scene.attachTo(canvas): the graph moves to `canvas` with every node and GPU
// resource it has; the previous canvas (if any) stops showing it.
void bro_scene_SceneGraph_attachTo(uint64_t sceneBits, uint64_t canvasBits) {
    const Rooted sceneVal(ev::fromBits(sceneBits));
    const Rooted canvasVal(ev::fromBits(canvasBits));
    HostSceneGraphCell* cell = sceneCellOfValue(sceneVal);
    scene::SceneGraph* g = cell ? cell->graph() : nullptr;
    if (!g) { ev::throwTypeError("attachTo: the scene has been destroyed"); return; }
    dom::Element* el = ev::isObject(canvasVal) ? hostElementOf(canvasVal) : nullptr;
    if (!el || util::toLower(el->tagName()) != "canvas") {
        ev::throwTypeError("attachTo: expected a <canvas> element");
        return;
    }
    auto* eng = hostEngine();
    dom::Document* doc = el->document();
    if (!eng || isChildRealm() || !doc ||
        eng->isWindowHostDocument(doc) || eng->isIframeDocument(doc)) {
        ev::throwTypeError("attachTo: a scene can only be shown by a canvas of the main document");
        return;
    }
    if (el->sceneGraph() == g && !g->parked()) return;  // already there
    if (el->sceneGraph() != nullptr) {
        ev::throwTypeError("attachTo: the canvas already shows another scene");
        return;
    }
    const std::string refusal = adoptSceneContext(el, canvasVal, sceneVal);
    if (!refusal.empty()) { ev::throwTypeError("attachTo: " + refusal); return; }
    if (!g->rebindCanvas(el)) { ev::throwTypeError("attachTo: the canvas could not take the scene"); return; }
    cell->canvas = el;
}

// scene.detach(): parks the graph on no canvas. Not rendered, not reclaimed
// with its old canvas; attachTo brings it back.
void bro_scene_SceneGraph_detach(void* self) {
    auto* cell = graphCellOf(self);
    scene::SceneGraph* g = cell ? cell->graph() : nullptr;
    if (!g) return;
    g->rebindCanvas(nullptr);
    cell->canvas = nullptr;
}

bool bro_scene_SceneGraph_attached_get(void* self) {
    auto* g = graphOf(self);
    return g && !g->parked();
}

bool bro_scene_SceneGraph_keepAlive_get(void* self) {
    auto* g = graphOf(self);
    return g && g->keepAlive();
}

void bro_scene_SceneGraph_keepAlive_set(void* self, bool v) {
    if (auto* g = graphOf(self)) g->setKeepAlive(v);
}

}  // extern "C"

namespace bro::bronze_host {

bool registerSceneAttachNatives(std::string* error) {
    using natives::fn;
    static const char* const G = "__bro_native.scene.SceneGraph";
    return fn("__bro_native.scene.SceneGraph_attachTo", (void*)&bro_scene_SceneGraph_attachTo, "void", {"dynamic", "dynamic"}, error) &&
           fn("__bro_native.scene.SceneGraph_detach", (void*)&bro_scene_SceneGraph_detach, "void", {G}, error) &&
           fn("__bro_native.scene.SceneGraph_attached_get", (void*)&bro_scene_SceneGraph_attached_get, "bool", {G}, error) &&
           fn("__bro_native.scene.SceneGraph_keepAlive_get", (void*)&bro_scene_SceneGraph_keepAlive_get, "bool", {G}, error) &&
           fn("__bro_native.scene.SceneGraph_keepAlive_set", (void*)&bro_scene_SceneGraph_keepAlive_set, "void", {G, "bool"}, error);
}

}  // namespace bro::bronze_host
