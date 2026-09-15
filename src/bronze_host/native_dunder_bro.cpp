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
#include "natives/dunder_bro/native_dunder_bro_decl.h"

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

}  // namespace bro::bronze_host

extern "C" {

using namespace bro::bronze_host;

void bro_dunder_bro_splash_dismiss(void) { splashDismiss(); }

double bro_dunder_bro_viewport_width_get(void) { return viewportWidth(); }
double bro_dunder_bro_viewport_height_get(void) { return viewportHeight(); }

double bro_dunder_bro_perf_fps_get(void) { return perfFps(); }
double bro_dunder_bro_perf_frameTime_get(void) { return perfFrameTime(); }
double bro_dunder_bro_perf_js_get(void) { return perfJs(); }
double bro_dunder_bro_perf_layout_get(void) { return perfLayout(); }
double bro_dunder_bro_perf_raster_get(void) { return perfRaster(); }
double bro_dunder_bro_perf_gpu_get(void) { return perfGpu(); }
double bro_dunder_bro_perf_draw_get(void) { return perfDraw(); }
int32_t bro_dunder_bro_perf_windowCount(void) { return windowCount(); }
double bro_dunder_bro_perf_windowId(int32_t index) { return windowId(index); }
const char* bro_dunder_bro_perf_windowTitle(int32_t index) { return windowTitle(index); }
double bro_dunder_bro_perf_windowWidth(int32_t index) { return windowWidth(index); }
double bro_dunder_bro_perf_windowHeight(int32_t index) { return windowHeight(index); }
bool bro_dunder_bro_perf_windowFocused(int32_t index) { return windowFocused(index); }
bool bro_dunder_bro_perf_windowMinimized(int32_t index) { return windowMinimized(index); }

#if BRO_WITH_3D
double bro_dunder_bro_scene_meshDrawn_get(void) { return sceneMeshDrawn(); }
double bro_dunder_bro_scene_meshCulled_get(void) { return sceneMeshCulled(); }
double bro_dunder_bro_scene_instancedDrawn_get(void) { return sceneInstancedDrawn(); }
double bro_dunder_bro_scene_instancedCulled_get(void) { return sceneInstancedCulled(); }
double bro_dunder_bro_scene_splatDrawn_get(void) { return sceneSplatDrawn(); }
double bro_dunder_bro_scene_splatCulled_get(void) { return sceneSplatCulled(); }
double bro_dunder_bro_scene_particlesDrawn_get(void) { return sceneParticlesDrawn(); }
double bro_dunder_bro_scene_particlesCulled_get(void) { return sceneParticlesCulled(); }
double bro_dunder_bro_scene_billboardsDrawn_get(void) { return sceneBillboardsDrawn(); }
double bro_dunder_bro_scene_billboardsCulled_get(void) { return sceneBillboardsCulled(); }
double bro_dunder_bro_scene_decalsDrawn_get(void) { return sceneDecalsDrawn(); }
double bro_dunder_bro_scene_decalsCulled_get(void) { return sceneDecalsCulled(); }
double bro_dunder_bro_scene_shadowDrawn_get(void) { return sceneShadowDrawn(); }
double bro_dunder_bro_scene_shadowCulled_get(void) { return sceneShadowCulled(); }
double bro_dunder_bro_scene_shadowTilesTotal_get(void) { return sceneShadowTilesTotal(); }
double bro_dunder_bro_scene_shadowTilesRendered_get(void) { return sceneShadowTilesRendered(); }
double bro_dunder_bro_scene_shadowTilesCached_get(void) { return sceneShadowTilesCached(); }
#endif

double bro_dunder_bro_bronze_heapUsedBytes_get(void) { return heapUsedBytes(); }
double bro_dunder_bro_bronze_heapCommittedBytes_get(void) { return heapCommittedBytes(); }
double bro_dunder_bro_bronze_heapReservedBytes_get(void) { return heapReservedBytes(); }
double bro_dunder_bro_bronze_gcCollections_get(void) { return gcCollections(); }
double bro_dunder_bro_bronze_gcPauseNs_get(void) { return gcPauseNs(); }
double bro_dunder_bro_bronze_shapeTransitions_get(void) { return shapeTransitions(); }

double bro_dunder_bro_menu_height(void) { return menuHeight(); }
const char* bro_dunder_bro_menu_treeJson(void) { return menuTreeJson(); }
void bro_dunder_bro_menu_click(const char* id) { menuClick(id); }

void bro_dunder_bro_settingsUI_show(const char* name) { settingsShow(name); }
const char* bro_dunder_bro_settingsUI_panelsJson(void) { return panelsJson(); }
const char* bro_dunder_bro_settingsUI_activePanel(void) { return activePanel(); }
void bro_dunder_bro_settingsUI_toggle(void) { settingsToggle(); }
bool bro_dunder_bro_settingsUI_isVisible(void) { return settingsIsVisible(); }
double bro_dunder_bro_settingsUI_contentTop(void) { return contentTop(); }

bool bro_dunder_bro_inspector_visible_get(void) { return inspVisible(); }
const char* bro_dunder_bro_inspector_dock_get(void) { return inspDock(); }
double bro_dunder_bro_inspector_width_get(void) { return inspWidth(); }
double bro_dunder_bro_inspector_height_get(void) { return inspHeight(); }
bool bro_dunder_bro_inspector_pickerMode_get(void) { return inspPickerMode(); }
const char* bro_dunder_bro_inspector_appTreeJson(int32_t maxDepth) { return inspAppTreeJson(maxDepth); }
const char* bro_dunder_bro_inspector_childrenJson(int32_t parentId) { return inspChildrenJson(parentId); }
const char* bro_dunder_bro_inspector_selectedJson(void) { return inspSelectedJson(); }
void bro_dunder_bro_inspector_select(int32_t id) { inspSelect(id); }
void bro_dunder_bro_inspector_setDock(const char* dock) { inspSetDock(dock); }
void bro_dunder_bro_inspector_setSize(double px) { inspSetSize(px); }
void bro_dunder_bro_inspector_setPickerMode(bool on) { inspSetPickerMode(on); }
void bro_dunder_bro_inspector_toggle(void) { inspToggle(); }

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_dunder_bro(std::string* error);

bool registerDunderBroNatives(std::string* error) {
    return registerNatives_dunder_bro(error);
}

}  // namespace bro::bronze_host
