// `__bro_native.{splash,viewport,perf,bronze,menu,settingsUI,inspector}` —
// the system panels' private surface (docs/system-panels.md), every member a
// read of engine state or a call into it. system/perf.html, nav.html,
// menu.html, splash.html, inspector.html and panel-runtime.js are the
// consumers; js/bro_core.js shapes what they read from these pieces.
//
// The perf telemetry is GETTERS OVER THE ENGINE'S OWN COUNTERS: the engine
// computes its 500 ms frame statistics for itself and answers them here, and
// perf.html reads them each tick and writes its own DOM. No C++ writes a
// panel's document.
//
// The secondary-window list crosses as scalar pieces by index over the live
// vector, the menu tree and the inspector's node trees as JSON the engine
// side already knows how to emit.

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "bronze_host/host_telemetry.h"
#include "engine/engine.h"
#if BRO_WITH_3D
#include "scene/scene_renderer.h"
#endif

#include <cstdio>
#include <string>

namespace bro::bronze_host {

namespace {

// ---- splash / viewport -----------------------------------------------------

void splashDismiss() { if (auto* eng = hostEngine()) eng->dismissSplash(); }

double viewportWidth() { auto* eng = hostEngine(); return eng ? eng->viewportWidth() : 1920; }
double viewportHeight() { auto* eng = hostEngine(); return eng ? eng->viewportHeight() : 1080; }

// ---- perf ------------------------------------------------------------------

double perfFps() { auto* eng = hostEngine(); return eng ? eng->perfFps() : 0.0; }
double perfFrameTime() { auto* eng = hostEngine(); return eng ? eng->perfFrameTimeMs() : 0.0; }
double perfJs() { auto* eng = hostEngine(); return eng ? eng->perfJsMs() : 0.0; }
double perfLayout() { auto* eng = hostEngine(); return eng ? eng->perfLayoutMs() : 0.0; }
double perfRaster() { auto* eng = hostEngine(); return eng ? eng->perfRasterMs() : 0.0; }
double perfGpu() { auto* eng = hostEngine(); return eng ? eng->perfGpuMs() : 0.0; }
double perfDraw() { auto* eng = hostEngine(); return eng ? eng->perfDrawMs() : 0.0; }

const engine::WindowHost* windowAt(int32_t i) {
    auto* eng = hostEngine();
    if (!eng || i < 0) return nullptr;
    const auto& hosts = eng->windowHosts();
    if (static_cast<size_t>(i) >= hosts.size()) return nullptr;
    return hosts[static_cast<size_t>(i)].get();
}

int32_t windowCount() {
    auto* eng = hostEngine();
    return eng ? static_cast<int32_t>(eng->windowHosts().size()) : 0;
}
double windowId(int32_t i) { auto* w = windowAt(i); return w ? static_cast<double>(w->id) : 0; }
const char* windowTitle(int32_t i) {
    auto* w = windowAt(i);
    return natives::strResult(w ? w->opts.title : std::string());
}
double windowWidth(int32_t i) { auto* w = windowAt(i); return w ? w->width : 0; }
double windowHeight(int32_t i) { auto* w = windowAt(i); return w ? w->height : 0; }
bool windowFocused(int32_t i) { auto* w = windowAt(i); return w && w->focused; }
bool windowMinimized(int32_t i) { auto* w = windowAt(i); return w && w->minimized; }

#if BRO_WITH_3D
scene::CullStats cull() {
    auto* eng = hostEngine();
    return eng ? eng->sceneCullStats() : scene::CullStats{};
}
double sceneMeshDrawn() { return cull().meshDrawn; }
double sceneMeshCulled() { return cull().meshCulled; }
double sceneInstancedDrawn() { return cull().instancedDrawn; }
double sceneInstancedCulled() { return cull().instancedCulled; }
double sceneSplatDrawn() { return cull().splatDrawn; }
double sceneSplatCulled() { return cull().splatCulled; }
double sceneParticlesDrawn() { return cull().particlesDrawn; }
double sceneParticlesCulled() { return cull().particlesCulled; }
double sceneBillboardsDrawn() { return cull().billboardsDrawn; }
double sceneBillboardsCulled() { return cull().billboardsCulled; }
double sceneDecalsDrawn() { return cull().decalsDrawn; }
double sceneDecalsCulled() { return cull().decalsCulled; }
double sceneShadowDrawn() { return cull().shadowDrawn; }
double sceneShadowCulled() { return cull().shadowCulled; }
double sceneShadowTilesTotal() { return cull().shadowTilesTotal; }
double sceneShadowTilesRendered() { return cull().shadowTilesRendered; }
double sceneShadowTilesCached() { return cull().shadowTilesCached; }
#endif

// ---- bronze (runtime telemetry) --------------------------------------------

double heapUsedBytes() { return static_cast<double>(getHostTelemetry().heapUsedBytes); }
double heapCommittedBytes() { return static_cast<double>(getHostTelemetry().heapCommittedBytes); }
double heapReservedBytes() { return static_cast<double>(getHostTelemetry().heapReservedBytes); }
double gcCollections() { return static_cast<double>(getHostTelemetry().gcCollections); }
double gcPauseNs() { return static_cast<double>(getHostTelemetry().gcPauseNs); }
double shapeTransitions() { return static_cast<double>(getHostTelemetry().shapeTransitions); }

// ---- menu ------------------------------------------------------------------

double menuHeight() { auto* eng = hostEngine(); return eng ? eng->menuBar().height : 28; }
const char* menuTreeJson() {
    auto* eng = hostEngine();
    return natives::strResult(eng ? eng->menuBar().toJSON() : std::string("[]"));
}
void menuClick(const char* id) { if (auto* eng = hostEngine()) eng->triggerMenuAction(id); }

// ---- settingsUI ------------------------------------------------------------

void jsonString(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
}

void settingsShow(const char* name) { if (auto* eng = hostEngine()) eng->showSystemPanel(name); }

// Every system document as {name, tabLabel, group}; the wrapper filters for
// getAllPanels (a tab label) and getSettingsPanels (the "settings" group).
const char* panelsJson() {
    auto* eng = hostEngine();
    std::string out = "[";
    if (eng) {
        bool first = true;
        for (const auto& d : eng->systemDocs()) {
            if (!first) out += ',';
            first = false;
            out += "{\"name\":"; jsonString(d.name, out);
            out += ",\"tabLabel\":"; jsonString(d.tabLabel, out);
            out += ",\"group\":"; jsonString(d.group, out);
            out += '}';
        }
    }
    out += ']';
    return natives::strResult(std::move(out));
}

const char* activePanel() {
    auto* eng = hostEngine();
    return natives::strResult(eng ? eng->systemActivePanel() : std::string());
}
void settingsToggle() { if (auto* eng = hostEngine()) eng->toggleSystemSettings(); }
bool settingsIsVisible() { auto* eng = hostEngine(); return eng && eng->isSystemVisible(); }
double contentTop() { auto* eng = hostEngine(); return eng ? eng->contentTop() : 0; }

// ---- inspector -------------------------------------------------------------

bool inspVisible() { auto* eng = hostEngine(); return eng && eng->inspector().visible; }
const char* inspDock() {
    auto* eng = hostEngine();
    return (eng && eng->inspector().dock == engine::InspectorDock::Bottom) ? "bottom" : "right";
}
double inspWidth() { auto* eng = hostEngine(); return eng ? eng->inspector().width : 320; }
double inspHeight() { auto* eng = hostEngine(); return eng ? eng->inspector().height : 280; }
bool inspPickerMode() { auto* eng = hostEngine(); return eng && eng->inspector().pickerMode; }

const char* inspAppTreeJson(int32_t maxDepth) {
    auto* eng = hostEngine();
    return natives::strResult(eng ? eng->inspectorAppTreeJson(maxDepth) : std::string("null"));
}
const char* inspChildrenJson(int32_t parentId) {
    auto* eng = hostEngine();
    return natives::strResult(eng ? eng->inspectorChildrenJson(parentId) : std::string("[]"));
}
const char* inspSelectedJson() {
    auto* eng = hostEngine();
    return natives::strResult(eng ? eng->inspectorSelectedJson() : std::string("null"));
}
void inspSelect(int32_t id) { if (auto* eng = hostEngine()) eng->inspectorSelectById(id); }
void inspSetDock(const char* dock) {
    if (auto* eng = hostEngine()) {
        eng->inspectorSetDock(std::string(dock) == "bottom" ? engine::InspectorDock::Bottom
                                                            : engine::InspectorDock::Right);
    }
}
void inspSetSize(double px) { if (auto* eng = hostEngine()) eng->inspectorSetSize(static_cast<int>(px)); }
void inspSetPickerMode(bool on) { if (auto* eng = hostEngine()) eng->inspectorSetPickerMode(on); }
void inspToggle() { if (auto* eng = hostEngine()) eng->toggleInspector(); }

}  // namespace

bool registerDunderBroNatives(std::string* error) {
    using namespace natives;
    auto p = [](auto f) { return reinterpret_cast<void*>(f); };
    bool ok =
        fn("__bro_native.splash.dismiss", p(&splashDismiss), "void", {}, error) &&
        getter("__bro_native.viewport.width", p(&viewportWidth), "f64", error) &&
        getter("__bro_native.viewport.height", p(&viewportHeight), "f64", error) &&
        getter("__bro_native.perf.fps", p(&perfFps), "f64", error) &&
        getter("__bro_native.perf.frameTime", p(&perfFrameTime), "f64", error) &&
        getter("__bro_native.perf.js", p(&perfJs), "f64", error) &&
        getter("__bro_native.perf.layout", p(&perfLayout), "f64", error) &&
        getter("__bro_native.perf.raster", p(&perfRaster), "f64", error) &&
        getter("__bro_native.perf.gpu", p(&perfGpu), "f64", error) &&
        getter("__bro_native.perf.draw", p(&perfDraw), "f64", error) &&
        fn("__bro_native.perf.windowCount", p(&windowCount), "i32", {}, error) &&
        fn("__bro_native.perf.windowId", p(&windowId), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowTitle", p(&windowTitle), "str", {"i32"}, error) &&
        fn("__bro_native.perf.windowWidth", p(&windowWidth), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowHeight", p(&windowHeight), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowFocused", p(&windowFocused), "bool", {"i32"}, error) &&
        fn("__bro_native.perf.windowMinimized", p(&windowMinimized), "bool", {"i32"}, error) &&
        getter("__bro_native.bronze.heapUsedBytes", p(&heapUsedBytes), "f64", error) &&
        getter("__bro_native.bronze.heapCommittedBytes", p(&heapCommittedBytes), "f64", error) &&
        getter("__bro_native.bronze.heapReservedBytes", p(&heapReservedBytes), "f64", error) &&
        getter("__bro_native.bronze.gcCollections", p(&gcCollections), "f64", error) &&
        getter("__bro_native.bronze.gcPauseNs", p(&gcPauseNs), "f64", error) &&
        getter("__bro_native.bronze.shapeTransitions", p(&shapeTransitions), "f64", error) &&
        fn("__bro_native.menu.height", p(&menuHeight), "f64", {}, error) &&
        fn("__bro_native.menu.treeJson", p(&menuTreeJson), "str", {}, error) &&
        fn("__bro_native.menu.click", p(&menuClick), "void", {"str"}, error) &&
        fn("__bro_native.settingsUI.show", p(&settingsShow), "void", {"str"}, error) &&
        fn("__bro_native.settingsUI.panelsJson", p(&panelsJson), "str", {}, error) &&
        fn("__bro_native.settingsUI.activePanel", p(&activePanel), "str", {}, error) &&
        fn("__bro_native.settingsUI.toggle", p(&settingsToggle), "void", {}, error) &&
        fn("__bro_native.settingsUI.isVisible", p(&settingsIsVisible), "bool", {}, error) &&
        fn("__bro_native.settingsUI.contentTop", p(&contentTop), "f64", {}, error) &&
        getter("__bro_native.inspector.visible", p(&inspVisible), "bool", error) &&
        getter("__bro_native.inspector.dock", p(&inspDock), "str", error) &&
        getter("__bro_native.inspector.width", p(&inspWidth), "f64", error) &&
        getter("__bro_native.inspector.height", p(&inspHeight), "f64", error) &&
        getter("__bro_native.inspector.pickerMode", p(&inspPickerMode), "bool", error) &&
        fn("__bro_native.inspector.appTreeJson", p(&inspAppTreeJson), "str", {"i32"}, error) &&
        fn("__bro_native.inspector.childrenJson", p(&inspChildrenJson), "str", {"i32"}, error) &&
        fn("__bro_native.inspector.selectedJson", p(&inspSelectedJson), "str", {}, error) &&
        fn("__bro_native.inspector.select", p(&inspSelect), "void", {"i32"}, error) &&
        fn("__bro_native.inspector.setDock", p(&inspSetDock), "void", {"str"}, error) &&
        fn("__bro_native.inspector.setSize", p(&inspSetSize), "void", {"f64"}, error) &&
        fn("__bro_native.inspector.setPickerMode", p(&inspSetPickerMode), "void", {"bool"}, error) &&
        fn("__bro_native.inspector.toggle", p(&inspToggle), "void", {}, error);
#if BRO_WITH_3D
    ok = ok &&
        getter("__bro_native.perf.scene.meshDrawn", p(&sceneMeshDrawn), "f64", error) &&
        getter("__bro_native.perf.scene.meshCulled", p(&sceneMeshCulled), "f64", error) &&
        getter("__bro_native.perf.scene.instancedDrawn", p(&sceneInstancedDrawn), "f64", error) &&
        getter("__bro_native.perf.scene.instancedCulled", p(&sceneInstancedCulled), "f64", error) &&
        getter("__bro_native.perf.scene.splatDrawn", p(&sceneSplatDrawn), "f64", error) &&
        getter("__bro_native.perf.scene.splatCulled", p(&sceneSplatCulled), "f64", error) &&
        getter("__bro_native.perf.scene.particlesDrawn", p(&sceneParticlesDrawn), "f64", error) &&
        getter("__bro_native.perf.scene.particlesCulled", p(&sceneParticlesCulled), "f64", error) &&
        getter("__bro_native.perf.scene.billboardsDrawn", p(&sceneBillboardsDrawn), "f64", error) &&
        getter("__bro_native.perf.scene.billboardsCulled", p(&sceneBillboardsCulled), "f64", error) &&
        getter("__bro_native.perf.scene.decalsDrawn", p(&sceneDecalsDrawn), "f64", error) &&
        getter("__bro_native.perf.scene.decalsCulled", p(&sceneDecalsCulled), "f64", error) &&
        getter("__bro_native.perf.scene.shadowDrawn", p(&sceneShadowDrawn), "f64", error) &&
        getter("__bro_native.perf.scene.shadowCulled", p(&sceneShadowCulled), "f64", error) &&
        getter("__bro_native.perf.scene.shadowTilesTotal", p(&sceneShadowTilesTotal), "f64", error) &&
        getter("__bro_native.perf.scene.shadowTilesRendered", p(&sceneShadowTilesRendered), "f64", error) &&
        getter("__bro_native.perf.scene.shadowTilesCached", p(&sceneShadowTilesCached), "f64", error);
#endif
    return ok;
}

}  // namespace bro::bronze_host
