// native_dunder_bro_register.cpp — registers every native of native_dunder_bro_decl.h with
// bronze (embed::registerNative) under the __bro_native roots the system panels' __bro.*
// reads (bronze, inspector, menu, perf, settingsUI, splash, viewport). Hand-maintained:
// each registration's signature matches its prototype there. Call registerNatives_dunder_bro from registerBroNatives
// (src/bronze_host/host_natives.h) on the thread that runs the program.
//
// Class members are Function-kind natives taking the handle first (the wrapper's
// receiver is dynamic, so a Method-kind registration would be unreachable from it);
// each class has one Constructor-kind registration, which mints the class, and its
// prototype is published as __bro_native.dunder_bro.<Class>Proto for js/bro_core.js to chain.

#include "native_dunder_bro_decl.h"

#include "embed/embed.h"

#include <initializer_list>
#include <string>

namespace bro::bronze_host {

namespace {

namespace ev = bronze::embed;

ev::NativeSignature sig(const char* ret, std::initializer_list<const char*> params, ev::NativeKind kind) {
    ev::NativeSignature s;
    s.returnType = ret;
    for (const char* p : params) s.paramTypes.emplace_back(p);
    s.kind = kind;
    return s;
}

bool fn(const char* path, void* f, const char* ret, std::initializer_list<const char*> params, std::string* error) {
    return ev::registerNative(path, f, sig(ret, params, ev::NativeKind::Function), error);
}

bool getter(const char* path, void* f, const char* ret, std::string* error) {
    return ev::registerNative(path, f, sig(ret, {}, ev::NativeKind::Getter), error);
}

template <typename F>
void* p(F* f) { return reinterpret_cast<void*>(f); }

}  // namespace

bool registerNatives_dunder_bro(std::string* error) {
    bool ok =
        fn("__bro_native.splash.dismiss", p(&bro_dunder_bro_splash_dismiss), "void", {}, error) &&
        getter("__bro_native.viewport.width", p(&bro_dunder_bro_viewport_width_get), "f64", error) &&
        getter("__bro_native.viewport.height", p(&bro_dunder_bro_viewport_height_get), "f64", error) &&
        getter("__bro_native.perf.fps", p(&bro_dunder_bro_perf_fps_get), "f64", error) &&
        getter("__bro_native.perf.frameTime", p(&bro_dunder_bro_perf_frameTime_get), "f64", error) &&
        getter("__bro_native.perf.js", p(&bro_dunder_bro_perf_js_get), "f64", error) &&
        getter("__bro_native.perf.layout", p(&bro_dunder_bro_perf_layout_get), "f64", error) &&
        getter("__bro_native.perf.raster", p(&bro_dunder_bro_perf_raster_get), "f64", error) &&
        getter("__bro_native.perf.gpu", p(&bro_dunder_bro_perf_gpu_get), "f64", error) &&
        getter("__bro_native.perf.draw", p(&bro_dunder_bro_perf_draw_get), "f64", error) &&
        fn("__bro_native.perf.windowCount", p(&bro_dunder_bro_perf_windowCount), "i32", {}, error) &&
        fn("__bro_native.perf.windowId", p(&bro_dunder_bro_perf_windowId), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowTitle", p(&bro_dunder_bro_perf_windowTitle), "str", {"i32"}, error) &&
        fn("__bro_native.perf.windowWidth", p(&bro_dunder_bro_perf_windowWidth), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowHeight", p(&bro_dunder_bro_perf_windowHeight), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowFocused", p(&bro_dunder_bro_perf_windowFocused), "bool", {"i32"}, error) &&
        fn("__bro_native.perf.windowMinimized", p(&bro_dunder_bro_perf_windowMinimized), "bool", {"i32"}, error) &&
        getter("__bro_native.bronze.heapUsedBytes", p(&bro_dunder_bro_bronze_heapUsedBytes_get), "f64", error) &&
        getter("__bro_native.bronze.heapCommittedBytes", p(&bro_dunder_bro_bronze_heapCommittedBytes_get), "f64", error) &&
        getter("__bro_native.bronze.heapReservedBytes", p(&bro_dunder_bro_bronze_heapReservedBytes_get), "f64", error) &&
        getter("__bro_native.bronze.gcCollections", p(&bro_dunder_bro_bronze_gcCollections_get), "f64", error) &&
        getter("__bro_native.bronze.gcPauseNs", p(&bro_dunder_bro_bronze_gcPauseNs_get), "f64", error) &&
        getter("__bro_native.bronze.shapeTransitions", p(&bro_dunder_bro_bronze_shapeTransitions_get), "f64", error) &&
        fn("__bro_native.menu.height", p(&bro_dunder_bro_menu_height), "f64", {}, error) &&
        fn("__bro_native.menu.treeJson", p(&bro_dunder_bro_menu_treeJson), "str", {}, error) &&
        fn("__bro_native.menu.click", p(&bro_dunder_bro_menu_click), "void", {"str"}, error) &&
        fn("__bro_native.settingsUI.show", p(&bro_dunder_bro_settingsUI_show), "void", {"str"}, error) &&
        fn("__bro_native.settingsUI.panelsJson", p(&bro_dunder_bro_settingsUI_panelsJson), "str", {}, error) &&
        fn("__bro_native.settingsUI.activePanel", p(&bro_dunder_bro_settingsUI_activePanel), "str", {}, error) &&
        fn("__bro_native.settingsUI.toggle", p(&bro_dunder_bro_settingsUI_toggle), "void", {}, error) &&
        fn("__bro_native.settingsUI.isVisible", p(&bro_dunder_bro_settingsUI_isVisible), "bool", {}, error) &&
        fn("__bro_native.settingsUI.contentTop", p(&bro_dunder_bro_settingsUI_contentTop), "f64", {}, error) &&
        getter("__bro_native.inspector.visible", p(&bro_dunder_bro_inspector_visible_get), "bool", error) &&
        getter("__bro_native.inspector.dock", p(&bro_dunder_bro_inspector_dock_get), "str", error) &&
        getter("__bro_native.inspector.width", p(&bro_dunder_bro_inspector_width_get), "f64", error) &&
        getter("__bro_native.inspector.height", p(&bro_dunder_bro_inspector_height_get), "f64", error) &&
        getter("__bro_native.inspector.pickerMode", p(&bro_dunder_bro_inspector_pickerMode_get), "bool", error) &&
        fn("__bro_native.inspector.appTreeJson", p(&bro_dunder_bro_inspector_appTreeJson), "str", {"i32"}, error) &&
        fn("__bro_native.inspector.childrenJson", p(&bro_dunder_bro_inspector_childrenJson), "str", {"i32"}, error) &&
        fn("__bro_native.inspector.selectedJson", p(&bro_dunder_bro_inspector_selectedJson), "str", {}, error) &&
        fn("__bro_native.inspector.select", p(&bro_dunder_bro_inspector_select), "void", {"i32"}, error) &&
        fn("__bro_native.inspector.setDock", p(&bro_dunder_bro_inspector_setDock), "void", {"str"}, error) &&
        fn("__bro_native.inspector.setSize", p(&bro_dunder_bro_inspector_setSize), "void", {"f64"}, error) &&
        fn("__bro_native.inspector.setPickerMode", p(&bro_dunder_bro_inspector_setPickerMode), "void", {"bool"}, error) &&
        fn("__bro_native.inspector.toggle", p(&bro_dunder_bro_inspector_toggle), "void", {}, error);
#if BRO_WITH_3D
    ok = ok &&
        getter("__bro_native.perf.scene.meshDrawn", p(&bro_dunder_bro_scene_meshDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.meshCulled", p(&bro_dunder_bro_scene_meshCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.instancedDrawn", p(&bro_dunder_bro_scene_instancedDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.instancedCulled", p(&bro_dunder_bro_scene_instancedCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.splatDrawn", p(&bro_dunder_bro_scene_splatDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.splatCulled", p(&bro_dunder_bro_scene_splatCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.particlesDrawn", p(&bro_dunder_bro_scene_particlesDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.particlesCulled", p(&bro_dunder_bro_scene_particlesCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.billboardsDrawn", p(&bro_dunder_bro_scene_billboardsDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.billboardsCulled", p(&bro_dunder_bro_scene_billboardsCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.decalsDrawn", p(&bro_dunder_bro_scene_decalsDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.decalsCulled", p(&bro_dunder_bro_scene_decalsCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowDrawn", p(&bro_dunder_bro_scene_shadowDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowCulled", p(&bro_dunder_bro_scene_shadowCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowTilesTotal", p(&bro_dunder_bro_scene_shadowTilesTotal_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowTilesRendered", p(&bro_dunder_bro_scene_shadowTilesRendered_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowTilesCached", p(&bro_dunder_bro_scene_shadowTilesCached_get), "f64", error);
#endif
    if (!ok) return false;
    return true;
}

}  // namespace bro::bronze_host
