// bro-headless — the stock headless driver.
//
// The driver itself lives in the engine (engine/headless_driver.cpp) so that
// an application embedding bro_engine gets the same scripting, screenshot and
// assertion surface with its own bindings installed, instead of reimplementing
// it. This binary supplies one hook set: `__host`, below.
//
// `__host` is not an app-facing API. It is the smallest possible host
// application — the same shape planet-bro and ffmpeg-bro have, and the shape
// broc's generated C++ has — expressed through nothing but the public engine
// surface, so tests/scene/test_host_scene_context.js can prove that a scene
// context built from C++ is the same thing canvas.getContext('scene') builds.
// If an engine change ever breaks that equivalence, that test fails, which is
// the point of putting it here rather than inside the engine.

#include "engine/headless_driver.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "js/dom_bindings.h"
#if BRO_WITH_3D
#include "js/scene_bindings.h"
#endif

#include <string>

namespace {

// Every entry point resolves the Engine the way a real host binding must: from
// the JSContext it was handed. There is no other route — the installer hook's
// signature is void(JSContext*), and in headless the host never sees the Engine
// at all (runHeadless constructs it internally).
bro::engine::Engine* hostEngine(JSContext* ctx) {
    return bro::engine::engineForContext(ctx);
}

// __host.createCanvas(id, cssWidth, cssHeight) -> Element
//
// Builds a <canvas> and puts it in the document entirely from C++: exactly what
// an AOT-compiled app has to do in place of the app.js that would otherwise
// have done it. Note the bare dom::Node::appendChild — a host has no JS binding
// to route through, and the element must still enter layout.
JSValue hostCreateCanvas(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* engine = hostEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "__host: no Engine for this realm");
    auto* doc = engine->document();
    if (!doc) return JS_ThrowInternalError(ctx, "__host: no document");
    auto* parent = doc->body() ? doc->body() : doc->documentElement();
    if (!parent) return JS_ThrowInternalError(ctx, "__host: document has no body");

    std::string id;
    if (argc >= 1) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { id = s; JS_FreeCString(ctx, s); }
    }
    int32_t w = 128, h = 128;
    if (argc >= 2) JS_ToInt32(ctx, &w, argv[1]);
    if (argc >= 3) JS_ToInt32(ctx, &h, argv[2]);

    auto* canvas = doc->createElement("canvas");
    if (!canvas) return JS_ThrowInternalError(ctx, "__host: createElement failed");
    if (!id.empty()) canvas->setAttribute("id", id);
    canvas->setAttribute("width", std::to_string(w));
    canvas->setAttribute("height", std::to_string(h));
    canvas->style().setProperty("width", std::to_string(w) + "px");
    canvas->style().setProperty("height", std::to_string(h) + "px");
    canvas->style().setProperty("display", "block");

    parent->appendChild(canvas);
    return bro::js::DomBindings::wrapElement(ctx, canvas);
}

// __host.sceneContext(canvasElement) -> SceneGraph | null
//
// The C++ half of canvas.getContext('scene'): the same Engine method the JS
// factory calls, reached the same way.
JSValue hostSceneContext(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* engine = hostEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "__host: no Engine for this realm");
    if (argc < 1) return JS_ThrowTypeError(ctx, "__host.sceneContext(canvas)");
    auto* el = static_cast<bro::dom::Element*>(
        bro::js::DomBindings::unwrapElement(ctx, argv[0]));
    if (!el) return JS_ThrowTypeError(ctx, "__host.sceneContext: not an Element");

    bro::scene::SceneGraph* graph = engine->createSceneContext(el);
    if (!graph) return JS_NULL;
#if BRO_WITH_3D
    return bro::js::SceneBindings::wrapSceneGraph(ctx, graph);
#else
    return JS_NULL;
#endif
}

// __host.sceneContextCount() -> number
//
// How many scene contexts the engine has registered. The test uses it to prove
// a second createSceneContext on the same canvas does not build a second one —
// a leaked registration would show up here and nowhere else.
JSValue hostSceneContextCount(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    auto* engine = hostEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "__host: no Engine for this realm");
    return JS_NewInt64(ctx, static_cast<int64_t>(engine->sceneContextCount()));
}

// Whether engineForContext() answered while the host installer itself was
// running, rather than only later from a called function. That ordering is the
// entire reason the engine back-pointer is registered before
// installCoreBindings instead of alongside the rest of the DOM bindings, and
// nothing else would catch a regression in it.
bool g_engineResolvedAtInstall = false;

JSValue hostEngineResolvedAtInstall(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    return JS_NewBool(ctx, g_engineResolvedAtInstall);
}

void installHostBindings(JSContext* ctx) {
    // The measurement this exists for — taken here, in the installer, where a
    // real host first wants an Engine*.
    g_engineResolvedAtInstall = (bro::engine::engineForContext(ctx) != nullptr);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue host = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, host, "createCanvas",
        JS_NewCFunction(ctx, hostCreateCanvas, "createCanvas", 3));
    JS_SetPropertyStr(ctx, host, "sceneContext",
        JS_NewCFunction(ctx, hostSceneContext, "sceneContext", 1));
    JS_SetPropertyStr(ctx, host, "sceneContextCount",
        JS_NewCFunction(ctx, hostSceneContextCount, "sceneContextCount", 0));
    JS_SetPropertyStr(ctx, host, "engineResolvedAtInstall",
        JS_NewCFunction(ctx, hostEngineResolvedAtInstall, "engineResolvedAtInstall", 0));
    JS_SetPropertyStr(ctx, global, "__host", host);
    JS_FreeValue(ctx, global);
}

}  // namespace

int main(int argc, char* argv[]) {
    bro::engine::HeadlessHooks hooks;
    hooks.installHostBindings = installHostBindings;
    return bro::engine::runHeadless(argc, argv, hooks);
}
