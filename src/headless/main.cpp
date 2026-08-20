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
#include "dom/event.h"
#include "dom/event_target.h"
#include "js/dom_bindings.h"
#if BRO_WITH_3D
#include "js/scene_bindings.h"
#endif
#if BRO_WITH_BRONZE
#include "bronze_host/app_module.h"
#include "bronze_host/gl_profile.h"
#endif

#include <string>
#include <vector>

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

// __host.sceneLink(canvasElement) -> { graph: bool, fboTexture: number }
//
// The engine's two back-pointers from a canvas Element to its scene graph, as
// booleans/ids rather than addresses. Nothing else can see them: they are
// opaque void*/GLuint fields the draw traversal reads to decide that this
// element is a composited 3D layer and which texture to composite. A graph that
// has been reclaimed must leave both cleared — a set flag over a destroyed
// graph is a compositor layer break aimed at freed memory.
JSValue hostSceneLink(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "__host.sceneLink(canvas)");
    auto* el = static_cast<bro::dom::Element*>(
        bro::js::DomBindings::unwrapElement(ctx, argv[0]));
    if (!el) return JS_ThrowTypeError(ctx, "__host.sceneLink: not an Element");
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "graph", JS_NewBool(ctx, el->sceneGraph() != nullptr));
    JS_SetPropertyStr(ctx, out, "fboTexture",
        JS_NewInt64(ctx, static_cast<int64_t>(el->sceneGraphFBOTexture())));
    return out;
}

// __host.hasJsListener(element, type) -> bool
//
// dom::Element's per-type JS-listener gate — the thing js::dispatchDomEvent
// consults before it will go near an element's JS wrapper at all. It has no DOM
// surface (the DOM has no "is anything listening for X" query, by design), and
// no behaviour distinguishes an accurate gate from one that only ever answers
// "maybe", which is precisely how it drifted out of step with reality: the
// removeEventListener binding never told the element anything.
JSValue hostHasJsListener(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "__host.hasJsListener(element, type)");
    auto* el = static_cast<bro::dom::Element*>(
        bro::js::DomBindings::unwrapElement(ctx, argv[0]));
    if (!el) return JS_ThrowTypeError(ctx, "__host.hasJsListener: not an Element");
    const char* type = JS_ToCString(ctx, argv[1]);
    if (!type) return JS_EXCEPTION;
    bool has = el->hasJsListener(type);
    JS_FreeCString(ctx, type);
    return JS_NewBool(ctx, has);
}

// ---------------------------------------------------------------------------
// __host.*Listener — the C++ event listener API, exercised the way a compiled
// app uses it.
//
// A compiled app lowers `window.addEventListener('resize', fn)` onto
// Engine::addWindowEventListener and `el.addEventListener('click', fn)` onto
// dom::Element::addEventListener; both hand back a dom::ListenerHandle it has
// to keep in order to detach again. Everything below is that, plus a log so a
// script can see what ran and in what order — a C++ callback has no other way
// to report to the test.
// ---------------------------------------------------------------------------

// What each invoked C++ listener appended, in call order. Shared with JS
// through __host.note() so a test can interleave both kinds in one sequence.
std::vector<std::string> g_callLog;

// Everything the last C++ listener saw on its dom::Event. This is the proof
// that a C++ listener gets a real event and not a stub.
struct SeenEvent {
    bool any = false;
    std::string type;
    std::string targetTag;
    std::string currentTargetTag;
    int eventPhase = 0;
    bool bubbles = false;
    bool cancelable = false;
    bool isTrusted = false;
    bool defaultPrevented = false;
    double clientX = 0, clientY = 0;
    std::string key;
} g_lastEvent;

// What a registered C++ listener should do when it runs, beyond logging.
struct ListenerBehaviour {
    bool preventDefault = false;
    bool stopPropagation = false;
    bool stopImmediatePropagation = false;
};

std::string elementLabel(bro::dom::Element* el) {
    if (!el) return "";
    std::string id = el->getAttribute("id");
    return id.empty() ? el->tagName() : ("#" + id);
}

bro::dom::ListenerOptions readOptions(JSContext* ctx, JSValueConst obj,
                                      ListenerBehaviour* behaviour) {
    bro::dom::ListenerOptions opts;
    if (!JS_IsObject(obj)) return opts;
    auto flag = [&](const char* name) {
        JSValue v = JS_GetPropertyStr(ctx, obj, name);
        bool b = JS_ToBool(ctx, v);
        JS_FreeValue(ctx, v);
        return b;
    };
    opts.capture = flag("capture");
    opts.once = flag("once");
    if (behaviour) {
        behaviour->preventDefault = flag("preventDefault");
        behaviour->stopPropagation = flag("stopPropagation");
        behaviour->stopImmediatePropagation = flag("stopImmediatePropagation");
    }
    return opts;
}

bro::dom::EventCallback makeLoggingListener(std::string tag,
                                            ListenerBehaviour behaviour) {
    return [tag, behaviour](bro::dom::Event& ev) {
        g_callLog.push_back(tag);

        g_lastEvent = SeenEvent{};
        g_lastEvent.any = true;
        g_lastEvent.type = ev.type();
        g_lastEvent.targetTag = elementLabel(ev.target());
        g_lastEvent.currentTargetTag = elementLabel(ev.currentTarget());
        g_lastEvent.eventPhase = ev.eventPhase();
        g_lastEvent.bubbles = ev.bubbles();
        g_lastEvent.cancelable = ev.cancelable();
        g_lastEvent.isTrusted = ev.isTrusted();
        g_lastEvent.defaultPrevented = ev.defaultPrevented();
        if (auto* me = dynamic_cast<bro::dom::MouseEvent*>(&ev)) {
            g_lastEvent.clientX = me->clientX();
            g_lastEvent.clientY = me->clientY();
        }
        if (auto* ke = dynamic_cast<bro::dom::KeyboardEvent*>(&ev)) {
            g_lastEvent.key = ke->key();
        }

        if (behaviour.preventDefault) ev.preventDefault();
        if (behaviour.stopPropagation) ev.stopPropagation();
        if (behaviour.stopImmediatePropagation) ev.stopImmediatePropagation();
    };
}

// __host.addWindowListener(type, tag, opts) -> handle id
JSValue hostAddWindowListener(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* engine = hostEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "__host: no Engine for this realm");
    if (argc < 2) return JS_ThrowTypeError(ctx, "__host.addWindowListener(type, tag, opts)");

    const char* typeC = JS_ToCString(ctx, argv[0]);
    const char* tagC = JS_ToCString(ctx, argv[1]);
    if (!typeC || !tagC) {
        if (typeC) JS_FreeCString(ctx, typeC);
        if (tagC) JS_FreeCString(ctx, tagC);
        return JS_ThrowTypeError(ctx, "__host.addWindowListener: type and tag must be strings");
    }
    std::string type(typeC), tag(tagC);
    JS_FreeCString(ctx, typeC);
    JS_FreeCString(ctx, tagC);

    ListenerBehaviour behaviour;
    auto opts = readOptions(ctx, argc >= 3 ? argv[2] : JS_UNDEFINED, &behaviour);

    auto handle = engine->addWindowEventListener(
        type, makeLoggingListener(std::move(tag), behaviour), opts);
    if (!handle) return JS_ThrowInternalError(ctx, "__host: addWindowEventListener failed");
    return JS_NewInt64(ctx, static_cast<int64_t>(handle.id));
}

// __host.removeWindowListener(id) -> bool
JSValue hostRemoveWindowListener(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* engine = hostEngine(ctx);
    if (!engine) return JS_ThrowInternalError(ctx, "__host: no Engine for this realm");
    int64_t id = 0;
    if (argc >= 1) JS_ToInt64(ctx, &id, argv[0]);
    return JS_NewBool(ctx,
        engine->removeWindowEventListener(bro::dom::ListenerHandle{static_cast<uint64_t>(id)}));
}

// __host.addElementListener(el, type, tag, opts) -> handle id
JSValue hostAddElementListener(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "__host.addElementListener(el, type, tag, opts)");
    auto* el = static_cast<bro::dom::Element*>(
        bro::js::DomBindings::unwrapElement(ctx, argv[0]));
    if (!el) return JS_ThrowTypeError(ctx, "__host.addElementListener: not an Element");

    const char* typeC = JS_ToCString(ctx, argv[1]);
    const char* tagC = JS_ToCString(ctx, argv[2]);
    if (!typeC || !tagC) {
        if (typeC) JS_FreeCString(ctx, typeC);
        if (tagC) JS_FreeCString(ctx, tagC);
        return JS_ThrowTypeError(ctx, "__host.addElementListener: type and tag must be strings");
    }
    std::string type(typeC), tag(tagC);
    JS_FreeCString(ctx, typeC);
    JS_FreeCString(ctx, tagC);

    ListenerBehaviour behaviour;
    auto opts = readOptions(ctx, argc >= 4 ? argv[3] : JS_UNDEFINED, &behaviour);

    auto handle = el->addEventListener(type, makeLoggingListener(std::move(tag), behaviour), opts);
    if (!handle) return JS_ThrowInternalError(ctx, "__host: addEventListener failed");
    return JS_NewInt64(ctx, static_cast<int64_t>(handle.id));
}

// __host.removeElementListener(el, id) -> bool
JSValue hostRemoveElementListener(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2) return JS_ThrowTypeError(ctx, "__host.removeElementListener(el, id)");
    auto* el = static_cast<bro::dom::Element*>(
        bro::js::DomBindings::unwrapElement(ctx, argv[0]));
    if (!el) return JS_ThrowTypeError(ctx, "__host.removeElementListener: not an Element");
    int64_t id = 0;
    JS_ToInt64(ctx, &id, argv[1]);
    return JS_NewBool(ctx,
        el->removeEventListener(bro::dom::ListenerHandle{static_cast<uint64_t>(id)}));
}

// __host.note(tag) — append to the same log the C++ listeners write to, so a
// JS listener's position in the sequence is directly comparable.
JSValue hostNote(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc >= 1) {
        const char* s = JS_ToCString(ctx, argv[0]);
        if (s) { g_callLog.push_back(s); JS_FreeCString(ctx, s); }
    }
    return JS_UNDEFINED;
}

// __host.log() -> string[]   /   __host.clearLog()
JSValue hostLog(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < g_callLog.size(); ++i)
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, g_callLog[i].c_str()));
    return arr;
}

JSValue hostClearLog(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    g_callLog.clear();
    g_lastEvent = SeenEvent{};
    return JS_UNDEFINED;
}

// __host.lastEvent() -> object | null — what the most recent C++ listener saw
// on its dom::Event.
JSValue hostLastEvent(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!g_lastEvent.any) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "type", JS_NewString(ctx, g_lastEvent.type.c_str()));
    JS_SetPropertyStr(ctx, o, "target", JS_NewString(ctx, g_lastEvent.targetTag.c_str()));
    JS_SetPropertyStr(ctx, o, "currentTarget",
                      JS_NewString(ctx, g_lastEvent.currentTargetTag.c_str()));
    JS_SetPropertyStr(ctx, o, "eventPhase", JS_NewInt32(ctx, g_lastEvent.eventPhase));
    JS_SetPropertyStr(ctx, o, "bubbles", JS_NewBool(ctx, g_lastEvent.bubbles));
    JS_SetPropertyStr(ctx, o, "cancelable", JS_NewBool(ctx, g_lastEvent.cancelable));
    JS_SetPropertyStr(ctx, o, "isTrusted", JS_NewBool(ctx, g_lastEvent.isTrusted));
    JS_SetPropertyStr(ctx, o, "defaultPrevented",
                      JS_NewBool(ctx, g_lastEvent.defaultPrevented));
    JS_SetPropertyStr(ctx, o, "clientX", JS_NewFloat64(ctx, g_lastEvent.clientX));
    JS_SetPropertyStr(ctx, o, "clientY", JS_NewFloat64(ctx, g_lastEvent.clientY));
    JS_SetPropertyStr(ctx, o, "key", JS_NewString(ctx, g_lastEvent.key.c_str()));
    return o;
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
    JS_SetPropertyStr(ctx, host, "sceneLink",
        JS_NewCFunction(ctx, hostSceneLink, "sceneLink", 1));
    JS_SetPropertyStr(ctx, host, "hasJsListener",
        JS_NewCFunction(ctx, hostHasJsListener, "hasJsListener", 2));
    JS_SetPropertyStr(ctx, host, "engineResolvedAtInstall",
        JS_NewCFunction(ctx, hostEngineResolvedAtInstall, "engineResolvedAtInstall", 0));
    JS_SetPropertyStr(ctx, host, "addWindowListener",
        JS_NewCFunction(ctx, hostAddWindowListener, "addWindowListener", 3));
    JS_SetPropertyStr(ctx, host, "removeWindowListener",
        JS_NewCFunction(ctx, hostRemoveWindowListener, "removeWindowListener", 1));
    JS_SetPropertyStr(ctx, host, "addElementListener",
        JS_NewCFunction(ctx, hostAddElementListener, "addElementListener", 4));
    JS_SetPropertyStr(ctx, host, "removeElementListener",
        JS_NewCFunction(ctx, hostRemoveElementListener, "removeElementListener", 2));
    JS_SetPropertyStr(ctx, host, "note",
        JS_NewCFunction(ctx, hostNote, "note", 1));
    JS_SetPropertyStr(ctx, host, "log",
        JS_NewCFunction(ctx, hostLog, "log", 0));
    JS_SetPropertyStr(ctx, host, "clearLog",
        JS_NewCFunction(ctx, hostClearLog, "clearLog", 0));
    JS_SetPropertyStr(ctx, host, "lastEvent",
        JS_NewCFunction(ctx, hostLastEvent, "lastEvent", 0));
    JS_SetPropertyStr(ctx, global, "__host", host);
    JS_FreeValue(ctx, global);
}

}  // namespace

int main(int argc, char* argv[]) {
    bro::engine::HeadlessHooks hooks;
    hooks.installHostBindings = installHostBindings;
#if BRO_WITH_BRONZE
    // The compiled half of an app directory, if it carries one. Same two hooks
    // bro.exe performs by hand around its Engine (src/main.cpp), which is the
    // point: one binary opens an interpreted app and a compiled one, and a
    // driver script cannot tell from the outside which it is stepping.
    //
    // Asked twice — once to answer the predicate before construction, once to
    // load — because the two hooks fire either side of the Engine and neither
    // owns state the other can read. It is one stat() each.
    hooks.providesCompiledApp = [](const std::string& appDir) {
        return bro::bronze_host::findAppModule(appDir).has_value();
    };
    hooks.afterEngine = [](bro::engine::Engine& engine) {
        if (auto modulePath = bro::bronze_host::findAppModule(engine.appDir()))
            bro::bronze_host::runAppModule(engine, *modulePath);
    };
    // The driver leaves through _exit(), so the BRO_GL_PROFILE table has to be
    // printed from here — an atexit registration would never run.
    hooks.beforeExit = [] { bro::bronze_host::hostProfileDump(); };
#endif
    return bro::engine::runHeadless(argc, argv, hooks);
}
