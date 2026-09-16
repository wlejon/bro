// bro's own JavaScript, shipped as bronze-compiled objects.
//
// Each module under js/ is compiled at build time by bro_compile_js
// (CMakeLists.txt) into an object with its own entry symbol and linked into
// this library, the way brokit ships its polyfills (../brokit/src/api). An
// installer here enters it through bronze::embed::runEntry — the module's top
// level runs in the realm the host has built so far — and then LIFTS the
// names it assigned onto `globalThis` into bronze's host-global registry.
// The lift is what makes them compiled-app globals: registeredHostGlobals()
// reads the registry back as the manifest every app is compiled against, and
// `bronze_global_get` asks the registry before the global object, so without
// it a compiled read of `MutationObserver` would miss a class that is right
// there on globalThis.
//
// Nothing here probes the filesystem for a .js at run time. The modules are
// in the binary.

#include "bronze_host/host_internal.h"
#include "embed/embed.h"

#if BRO_WITH_AUDIO
#include <broaudio/api.h>
#endif
#if BRO_WITH_FLORA
#include <broflora/api/api.h>
#endif
#if BRO_WITH_TENSOR
#include <brotensor/api.h>
#endif
#if BRO_WITH_LM
#include <brolm/api.h>
#endif
#if BRO_WITH_SOUNDML
#include <brosoundml/api.h>
#endif
#if BRO_WITH_DIFFUSION
#include <brodiffusion/api.h>
#endif

extern "C" void bro_observers_main();
extern "C" void bro_events_main();
extern "C" void bro_net_sync_main();
extern "C" void bro_image_gpu_main();
extern "C" void bro_core_main();
extern "C" void bro_mesh_main();
extern "C" void bro_net_main();
extern "C" void bro_rigging_main();
extern "C" void bro_physics_main();
extern "C" void bro_terrain_main();
extern "C" void bro_clipmap_main();
extern "C" void bro_tile_world_main();
extern "C" void bro_lighting_main();
extern "C" void bro_gizmo_main();
extern "C" void bro_animation_main();
extern "C" void bro_scene_main();
extern "C" void bro_motion_main();
extern "C" void bro_vision_main();
extern "C" void bro_impostor_main();

namespace bro::bronze_host {

namespace {

void adoptGlobalProperty(const char* name) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    Value v = ev::getProperty(gt.value, name);
    if (!ev::isUndefined(v)) ev::registerGlobal(name, v);
}

void mountImageGpu() {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    Value broVal = ev::getProperty(gt.value, "bro");
    if (!ev::isObject(broVal)) return;
    Value imgVal = ev::getProperty(broVal, "image");
    Value gpuVal = ev::getProperty(gt.value, "__bro_image_gpu");
    if (ev::isObject(imgVal) && !ev::isUndefined(gpuVal)) {
        ev::setProperty(imgVal, "gpu", gpuVal);
    }
}

}  // namespace

void installObserversModule() {
    // The module reads `__bro_observers` at its top level, so the hook object
    // is registered first (host_observer_hooks.cpp).
    installObserverHooks();
    bronze::embed::runEntry(bro_observers_main);
    adoptGlobalProperty("MutationObserver");
    adoptGlobalProperty("MutationRecord");
    adoptGlobalProperty("ResizeObserver");
    adoptGlobalProperty("ResizeObserverEntry");
    adoptGlobalProperty("ResizeObserverSize");
    adoptGlobalProperty("IntersectionObserver");
    adoptGlobalProperty("IntersectionObserverEntry");
}

// js/events.js extends brokit's `Event`, so it runs after installBrokitGlobals.
void installEventsModule() {
    bronze::embed::runEntry(bro_events_main);
    for (const char* name : {"UIEvent", "MouseEvent", "KeyboardEvent", "InputEvent", "FocusEvent",
                             "WheelEvent", "PointerEvent", "DragEvent", "CompositionEvent",
                             "AnimationEvent", "TransitionEvent", "ClipboardEvent", "SubmitEvent",
                             "ErrorEvent", "ProgressEvent", "PromiseRejectionEvent"}) {
        adoptGlobalProperty(name);
    }
}

void installNetSyncModule() {
    bronze::embed::runEntry(bro_net_sync_main);
    adoptGlobalProperty("__bro_net_sync");
}

void installImageGpuModule() {
    bronze::embed::runEntry(bro_image_gpu_main);
    adoptGlobalProperty("__bro_image_gpu");
    mountImageGpu();
}

// Nothing to lift: bro_core.js defines members ON the three roots
// host_bro_root.cpp registered before this runs, and adds no global of its
// own. It is compiled against the native manifest (--native-manifest) as
// well as js/module.globals, so its `__bro_native.x.y` spellings are direct
// native calls.
void installBroCoreModule() {
    bronze::embed::runEntry(bro_core_main);
}

// mesh.js defines `Mesh` and `MeshBVH` on globalThis (and on bro.mesh); the
// two classes are lifted so a compiled app's bare `Mesh` is a host global.
// Compiled against the native manifest like bro_core.js.
void installMeshModule() {
    bronze::embed::runEntry(bro_mesh_main);
    adoptGlobalProperty("Mesh");
    adoptGlobalProperty("MeshBVH");
}

void installNetModule() {
    bronze::embed::runEntry(bro_net_main);
}

void installRiggingModule() {
    bronze::embed::runEntry(bro_rigging_main);
    adoptGlobalProperty("SkinData");
    adoptGlobalProperty("Skeleton");
    adoptGlobalProperty("Pose");
    adoptGlobalProperty("Animation");
    adoptGlobalProperty("SkeletalAnimation");
    adoptGlobalProperty("RigSpec");
    adoptGlobalProperty("VoxelChunk");
    adoptGlobalProperty("IK");
    adoptGlobalProperty("Rig");
}

void installPhysicsModule() {
    bronze::embed::runEntry(bro_physics_main);
    adoptGlobalProperty("Physics");
    adoptGlobalProperty("PhysicsCharacter");
    adoptGlobalProperty("PhysicsSoftBody");
    adoptGlobalProperty("PhysicsVehicle");
    adoptGlobalProperty("PhysicsRagdoll");
    adoptGlobalProperty("PhysicsWorldHandle");
}

void installTerrainModule() {
    bronze::embed::runEntry(bro_terrain_main);
    adoptGlobalProperty("Terrain");
}

void installClipmapModule() {
    bronze::embed::runEntry(bro_clipmap_main);
    adoptGlobalProperty("ClipmapTerrain");
}

void installTileWorldModule() {
    bronze::embed::runEntry(bro_tile_world_main);
    adoptGlobalProperty("TileWorld");
}

void installLightingModule() {
    bronze::embed::runEntry(bro_lighting_main);
}

void installGizmoModule() {
    bronze::embed::runEntry(bro_gizmo_main);
}

void installAnimationModule() {
    bronze::embed::runEntry(bro_animation_main);
    adoptGlobalProperty("Tween");
    adoptGlobalProperty("AnimationPlayer");
}

void installSceneModule() {
    bronze::embed::runEntry(bro_scene_main);
    adoptGlobalProperty("SceneNode");
    adoptGlobalProperty("SceneGraph");
}

void installLmModule() {
#if BRO_WITH_LM
    brolm::api::installLM();
    adoptGlobalProperty("AsyncHandle");
    adoptGlobalProperty("QwenTokenizer");
    adoptGlobalProperty("MistralTokenizer");
    adoptGlobalProperty("GemmaTokenizer");
    adoptGlobalProperty("LMModel");
    adoptGlobalProperty("Qwen35Model");
    adoptGlobalProperty("Qwen3VLModel");
    adoptGlobalProperty("NllbModel");
    adoptGlobalProperty("ClipModel");
    adoptGlobalProperty("T5Model");
#endif
}

void installRaveModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installMotionModule() {
    bronze::embed::runEntry(bro_motion_main);
}

void installMicModule() {
#if BRO_WITH_AUDIO
    broaudio::api::installMic();
#endif
}

void installSenseModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installGestureModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installWakeModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installKwsModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installListenModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installTriposplatModule() {
#if BRO_WITH_DIFFUSION
    brodiffusion::api::installDiffusion();
#endif
}

void installDiffusionModule() {
#if BRO_WITH_DIFFUSION
    brodiffusion::api::installDiffusion();
#endif
}

void installVisionModule() {
    bronze::embed::runEntry(bro_vision_main);
}

void installDiarModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installSttModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installTtsModule() {
#if BRO_WITH_SOUNDML
    brosoundml::api::installSoundML();
#endif
}

void installFloraModule() {
#if BRO_WITH_FLORA
    broflora::api::installFlora();
    adoptGlobalProperty("FloraWorld");
#endif
}

void installTensorModule() {
#if BRO_WITH_TENSOR
    brotensor::api::installTensor();
    adoptGlobalProperty("GpuTensor");
#endif
}

void installImpostorModule() {
    bronze::embed::runEntry(bro_impostor_main);
}

}  // namespace bro::bronze_host
