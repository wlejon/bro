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
#include "bronze_host/native_table.h"
#include "bronze_host/host_natives.h"

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

}  // namespace

bool registerNatives_dunder_bro(std::string* error) {
    bool ok =
        fn("__bro_native.splash.dismiss", BRO_NATIVE_FN(bro_dunder_bro_splash_dismiss), "void", {}, error) &&
        getter("__bro_native.viewport.width", BRO_NATIVE_FN(bro_dunder_bro_viewport_width_get), "f64", error) &&
        getter("__bro_native.viewport.height", BRO_NATIVE_FN(bro_dunder_bro_viewport_height_get), "f64", error) &&
        getter("__bro_native.perf.fps", BRO_NATIVE_FN(bro_dunder_bro_perf_fps_get), "f64", error) &&
        getter("__bro_native.perf.frameTime", BRO_NATIVE_FN(bro_dunder_bro_perf_frameTime_get), "f64", error) &&
        getter("__bro_native.perf.js", BRO_NATIVE_FN(bro_dunder_bro_perf_js_get), "f64", error) &&
        getter("__bro_native.perf.layout", BRO_NATIVE_FN(bro_dunder_bro_perf_layout_get), "f64", error) &&
        getter("__bro_native.perf.raster", BRO_NATIVE_FN(bro_dunder_bro_perf_raster_get), "f64", error) &&
        getter("__bro_native.perf.gpu", BRO_NATIVE_FN(bro_dunder_bro_perf_gpu_get), "f64", error) &&
        getter("__bro_native.perf.draw", BRO_NATIVE_FN(bro_dunder_bro_perf_draw_get), "f64", error) &&
        fn("__bro_native.perf.windowCount", BRO_NATIVE_FN(bro_dunder_bro_perf_windowCount), "i32", {}, error) &&
        fn("__bro_native.perf.windowId", BRO_NATIVE_FN(bro_dunder_bro_perf_windowId), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowTitle", BRO_NATIVE_FN(bro_dunder_bro_perf_windowTitle), "str", {"i32"}, error) &&
        fn("__bro_native.perf.windowWidth", BRO_NATIVE_FN(bro_dunder_bro_perf_windowWidth), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowHeight", BRO_NATIVE_FN(bro_dunder_bro_perf_windowHeight), "f64", {"i32"}, error) &&
        fn("__bro_native.perf.windowFocused", BRO_NATIVE_FN(bro_dunder_bro_perf_windowFocused), "bool", {"i32"}, error) &&
        fn("__bro_native.perf.windowMinimized", BRO_NATIVE_FN(bro_dunder_bro_perf_windowMinimized), "bool", {"i32"}, error) &&
        getter("__bro_native.bronze.heapUsedBytes", BRO_NATIVE_FN(bro_dunder_bro_bronze_heapUsedBytes_get), "f64", error) &&
        getter("__bro_native.bronze.heapCommittedBytes", BRO_NATIVE_FN(bro_dunder_bro_bronze_heapCommittedBytes_get), "f64", error) &&
        getter("__bro_native.bronze.heapReservedBytes", BRO_NATIVE_FN(bro_dunder_bro_bronze_heapReservedBytes_get), "f64", error) &&
        getter("__bro_native.bronze.gcCollections", BRO_NATIVE_FN(bro_dunder_bro_bronze_gcCollections_get), "f64", error) &&
        getter("__bro_native.bronze.gcPauseNs", BRO_NATIVE_FN(bro_dunder_bro_bronze_gcPauseNs_get), "f64", error) &&
        getter("__bro_native.bronze.shapeTransitions", BRO_NATIVE_FN(bro_dunder_bro_bronze_shapeTransitions_get), "f64", error) &&
        fn("__bro_native.menu.height", BRO_NATIVE_FN(bro_dunder_bro_menu_height), "f64", {}, error) &&
        fn("__bro_native.menu.treeJson", BRO_NATIVE_FN(bro_dunder_bro_menu_treeJson), "str", {}, error) &&
        fn("__bro_native.menu.click", BRO_NATIVE_FN(bro_dunder_bro_menu_click), "void", {"str"}, error) &&
        fn("__bro_native.settingsUI.show", BRO_NATIVE_FN(bro_dunder_bro_settingsUI_show), "void", {"str"}, error) &&
        fn("__bro_native.settingsUI.panelsJson", BRO_NATIVE_FN(bro_dunder_bro_settingsUI_panelsJson), "str", {}, error) &&
        fn("__bro_native.settingsUI.activePanel", BRO_NATIVE_FN(bro_dunder_bro_settingsUI_activePanel), "str", {}, error) &&
        fn("__bro_native.settingsUI.toggle", BRO_NATIVE_FN(bro_dunder_bro_settingsUI_toggle), "void", {}, error) &&
        fn("__bro_native.settingsUI.isVisible", BRO_NATIVE_FN(bro_dunder_bro_settingsUI_isVisible), "bool", {}, error) &&
        fn("__bro_native.settingsUI.contentTop", BRO_NATIVE_FN(bro_dunder_bro_settingsUI_contentTop), "f64", {}, error) &&
        getter("__bro_native.inspector.visible", BRO_NATIVE_FN(bro_dunder_bro_inspector_visible_get), "bool", error) &&
        getter("__bro_native.inspector.dock", BRO_NATIVE_FN(bro_dunder_bro_inspector_dock_get), "str", error) &&
        getter("__bro_native.inspector.width", BRO_NATIVE_FN(bro_dunder_bro_inspector_width_get), "f64", error) &&
        getter("__bro_native.inspector.height", BRO_NATIVE_FN(bro_dunder_bro_inspector_height_get), "f64", error) &&
        getter("__bro_native.inspector.pickerMode", BRO_NATIVE_FN(bro_dunder_bro_inspector_pickerMode_get), "bool", error) &&
        fn("__bro_native.inspector.appTreeJson", BRO_NATIVE_FN(bro_dunder_bro_inspector_appTreeJson), "str", {"i32"}, error) &&
        fn("__bro_native.inspector.childrenJson", BRO_NATIVE_FN(bro_dunder_bro_inspector_childrenJson), "str", {"i32"}, error) &&
        fn("__bro_native.inspector.selectedJson", BRO_NATIVE_FN(bro_dunder_bro_inspector_selectedJson), "str", {}, error) &&
        fn("__bro_native.inspector.select", BRO_NATIVE_FN(bro_dunder_bro_inspector_select), "void", {"i32"}, error) &&
        fn("__bro_native.inspector.setDock", BRO_NATIVE_FN(bro_dunder_bro_inspector_setDock), "void", {"str"}, error) &&
        fn("__bro_native.inspector.setSize", BRO_NATIVE_FN(bro_dunder_bro_inspector_setSize), "void", {"f64"}, error) &&
        fn("__bro_native.inspector.setPickerMode", BRO_NATIVE_FN(bro_dunder_bro_inspector_setPickerMode), "void", {"bool"}, error) &&
        fn("__bro_native.inspector.toggle", BRO_NATIVE_FN(bro_dunder_bro_inspector_toggle), "void", {}, error);
#if BRO_WITH_3D
    ok = ok &&
        getter("__bro_native.perf.scene.meshDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_meshDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.meshCulled", BRO_NATIVE_FN(bro_dunder_bro_scene_meshCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.instancedDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_instancedDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.instancedCulled", BRO_NATIVE_FN(bro_dunder_bro_scene_instancedCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.splatDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_splatDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.splatCulled", BRO_NATIVE_FN(bro_dunder_bro_scene_splatCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.particlesDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_particlesDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.particlesCulled", BRO_NATIVE_FN(bro_dunder_bro_scene_particlesCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.billboardsDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_billboardsDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.billboardsCulled", BRO_NATIVE_FN(bro_dunder_bro_scene_billboardsCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.decalsDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_decalsDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.decalsCulled", BRO_NATIVE_FN(bro_dunder_bro_scene_decalsCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_shadowDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowCulled", BRO_NATIVE_FN(bro_dunder_bro_scene_shadowCulled_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowPagesNeeded", BRO_NATIVE_FN(bro_dunder_bro_scene_shadowPagesNeeded_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowPagesDrawn", BRO_NATIVE_FN(bro_dunder_bro_scene_shadowPagesDrawn_get), "f64", error) &&
        getter("__bro_native.perf.scene.shadowPagesResident", BRO_NATIVE_FN(bro_dunder_bro_scene_shadowPagesResident_get), "f64", error);
#endif
    if (!ok) return false;
    return true;
}

bool registerDunderBroNatives(std::string* error) {
    return registerNatives_dunder_bro(error);
}

}  // namespace bro::bronze_host
