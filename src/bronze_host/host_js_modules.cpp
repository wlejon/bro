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
//
// The sibling libraries' APIs (bro.mesh, bro.lm, bro.stt, bro.tensor, ...)
// are not entered from here: host_sibling_apis.cpp installs each of them
// exactly once from installBroRoots. There is no per-subsystem shell for
// them any more, because the shells were what called brosoundml's one
// installer nine times.

#include "bronze_host/host_js_modules.h"
#include "bronze_host/host_natives.h"
#include "embed/embed.h"

#include <span>
#include <string>
#include <vector>

extern "C" void bro_observers_main();
extern "C" void bro_events_main();
extern "C" void bro_clipboard_main();
extern "C" void bro_notification_main();
extern "C" void bro_net_sync_main();
extern "C" void bro_image_gpu_main();
extern "C" void bro_vision_wildlife_main();
extern "C" void bro_core_main();
extern "C" void bro_net_main();
extern "C" void bro_physics_main();
extern "C" void bro_terrain_main();
extern "C" void bro_clipmap_main();
extern "C" void bro_tile_world_main();
extern "C" void bro_lighting_main();
extern "C" void bro_gizmo_main();
extern "C" void bro_animation_main();
extern "C" void bro_scene_main();
extern "C" void bro_scene_graph_main();
extern "C" void bro_scene_extras_main();
extern "C" void bro_scene_grass_main();
extern "C" void bro_scene_forest_main();
extern "C" void bro_scene_plants_main();
extern "C" void bro_motion_main();
extern "C" void bro_server_main();
extern "C" void bro_impostor_main();
extern "C" void bro_profiler_main();
extern "C" void bro_models_main();

namespace bro::bronze_host {

namespace {

// bro.<ns>.<member> = globalThis.<global>, when both exist.
void mountOnNamespace(const char* ns, const char* member, const char* global) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    // globalThis and bro.<ns> are held across property reads, which allocate.
    const ev::Persistent globalObj(gt.value);
    Value broVal = ev::getProperty(globalObj.get(), "bro");
    if (!ev::isObject(broVal)) return;
    const ev::Persistent nsVal(ev::getProperty(broVal, ns));
    Value v = ev::getProperty(globalObj.get(), global);
    if (ev::isObject(nsVal.get()) && !ev::isUndefined(v)) {
        ev::setProperty(nsVal.get(), member, v);
    }
}

}  // namespace

void adoptGlobalProperty(const char* name) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    Value v = ev::getProperty(gt.value, name);
    if (!ev::isUndefined(v)) ev::registerGlobal(name, v);
}

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
                             "ErrorEvent", "HashChangeEvent", "PopStateEvent", "FormDataEvent",
                             "ProgressEvent", "PromiseRejectionEvent", "StorageEvent"}) {
        adoptGlobalProperty(name);
    }
    // events.js also puts the 25 legacy `*_ERR` codes and `code` on the
    // DOMException brokit installed; re-registering the (same) object keeps a
    // compiled bare `DOMException` resolving to it.
    adoptGlobalProperty("DOMException");
}

// js/clipboard.js puts write()/read() on the navigator.clipboard that
// installNavigatorGlobal made and builds on brokit's Blob, so it runs after
// both.
void installClipboardModule() {
    bronze::embed::runEntry(bro_clipboard_main);
    adoptGlobalProperty("ClipboardItem");
}

// js/notification.js reads bro.window.notify at the point of use, so it
// loads anywhere after brokit's Event.
void installNotificationModule() {
    bronze::embed::runEntry(bro_notification_main);
    adoptGlobalProperty("Notification");
    // What host_notification.cpp calls with each click and dismissal.
    adoptGlobalProperty("__bro_notificationActivated");
}

void installNetSyncModule() {
    bronze::embed::runEntry(bro_net_sync_main);
    adoptGlobalProperty("__bro_net_sync");
}

void installImageGpuModule() {
    bronze::embed::runEntry(bro_image_gpu_main);
    adoptGlobalProperty("__bro_image_gpu");
    mountOnNamespace("image", "gpu", "__bro_image_gpu");
}

// js/vision_wildlife.js reads bro.vision / bro.lm / bro.image / bro.models
// only when called, so it needs nothing but the `bro.vision` it mounts on.
void installVisionWildlifeModule() {
    bronze::embed::runEntry(bro_vision_wildlife_main);
    adoptGlobalProperty("__bro_vision_wildlife");
    mountOnNamespace("vision", "wildlife", "__bro_vision_wildlife");
}

// Nothing to lift: bro_core.js defines members ON the three roots
// host_bro_root.cpp registered before this runs, and adds no global of its
// own. It is compiled against the native manifest (--native-manifest) as
// well as js/module.globals, so its `__bro_native.x.y` spellings are direct
// native calls.
void installBroCoreModule() {
    bronze::embed::runEntry(bro_core_main);
}

void installNetModule() {
    bronze::embed::runEntry(bro_net_main);
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
    // js/scene_graph.js reads SceneGraph off globalThis, which scene.js has
    // just put there, and installs its members: the node factories, the
    // environment and post-processing setters, raycast, capture.
    bronze::embed::runEntry(bro_scene_graph_main);
    // js/scene_extras.js reads SceneNode / SceneGraph off globalThis the same
    // way, adds the per-type accessors and the instanced-mesh operations over
    // native_scene_extras.cpp, and wraps the create* factories scene_graph.js
    // has just installed — so it runs last.
    bronze::embed::runEntry(bro_scene_extras_main);
    // js/scene_grass.js: createGrass and the grass node's members.
    bronze::embed::runEntry(bro_scene_grass_main);
    // js/scene_forest.js: createForest and the forest node's members.
    bronze::embed::runEntry(bro_scene_forest_main);
    // js/scene_plants.js: createPlants and the plants node's members.
    bronze::embed::runEntry(bro_scene_plants_main);
}

void installMotionModule() {
    bronze::embed::runEntry(bro_motion_main);
}

// js/server.js mounts bro.server over the server natives; entered by the
// main realm and by every Worker realm (host_bro_root.cpp), each over its
// own `bro` root read off globalThis.
void installServerModule() {
    bronze::embed::runEntry(bro_server_main);
}

// impostor.js reads `Mesh` (bromesh's, installed with the roots) only when
// an impostor is built, never at load.
void installImpostorModule() {
    bronze::embed::runEntry(bro_impostor_main);
}

// js/profiler.js mounts bro.profiler over __bro_native.profiler; main realm
// only (installBroRoots).
void installProfilerModule() {
    bronze::embed::runEntry(bro_profiler_main);
}

// js/models.js mounts bro.models over __bro_native.models; main realm only
// (installBroRoots), since the setting it reads is the engine's.
void installModelsModule() {
    bronze::embed::runEntry(bro_models_main);
}

// ---- deferred groups ---------------------------------------------------------
//
// installDeferredModules (host_js_modules.h). A group runs once, from
// whichever trigger a program reaches first: one of its globals (bronze
// forces a lazy global on its first read), one of its `bro.<ns>` accessors,
// or a host path that needs it (ensure3DModules). Its install is the same
// sequence of entries the eager boot ran, in the same order.

namespace {

// A `bro.<ns>` (or `bro.image.gpu`, ...) member standing in as an accessor
// until its group runs, and what the member held before (the empty
// placeholder makeRoot made, or nothing).
struct DeferredMember {
    ev::Persistent parent;
    std::string member;
    bool hadValue = false;
    ev::Persistent previous;
};

struct ModuleGroup {
    const char* label = "";
    bool started = false;
    void (*install)() = nullptr;
    std::vector<DeferredMember> members;
    // The group's members and globals, for isDeferredName; emptied when it runs.
    std::vector<std::string> names;
};

// Never freed, like every root this layer keeps for the process: their
// Persistents must not be destroyed by static teardown after the heap.
std::vector<ModuleGroup*>& groups() {
    static auto* all = new std::vector<ModuleGroup*>();
    return *all;
}

ModuleGroup* g3d = nullptr;

void runGroup(ModuleGroup& g) {
    if (g.started) return;
    g.started = true;
    ev::runAsLazyInit([&g] {
        // The members as data properties again, before any module reads them:
        // a module mounts onto `bro.<ns>` (makeRoot's placeholder) the way it
        // did at boot.
        for (DeferredMember& m : g.members) {
            ev::deleteProperty(m.parent.get(), m.member);
            if (m.hadValue) m.parent.set(ev::setProperty(m.parent.get(), m.member, m.previous.get()));
        }
        g.install();
        g.names.clear();
        // `available: true` on what the group mounted.
        markAvailableNamespaces();
    });
}

void deferMember(ModuleGroup& g, Value parentIn, const char* member) {
    if (!ev::isObject(parentIn)) return;
    DeferredMember m;
    m.parent.set(parentIn);
    m.member = member;
    {
        Value cur = ev::getProperty(m.parent.get(), member);
        if (!ev::isUndefined(cur)) {
            m.hadValue = true;
            m.previous.set(cur);
        }
    }
    ModuleGroup* gp = &g;
    const std::string name(member);
    // The parent is read back through its own Persistent in the group, which
    // outlives these functions.
    const size_t index = g.members.size();
    // Each callback copies its captures first: running the group deletes the
    // accessor, after which a collection may free the function object and
    // this closure with it while the callback is still running.
    const ev::Persistent getter(ev::makeFunction(
        [gp, index](Value, std::span<const Value>) -> Value {
            ModuleGroup* group = gp;
            const size_t at = index;
            runGroup(*group);
            const DeferredMember& dm = group->members[at];
            return ev::getProperty(dm.parent.get(), dm.member);
        },
        0));
    const ev::Persistent setter(ev::makeFunction(
        [gp, index](Value, std::span<const Value> a) -> Value {
            ModuleGroup* group = gp;
            const size_t at = index;
            const ev::Persistent v(a.empty() ? ev::undefined() : a[0]);
            runGroup(*group);
            DeferredMember& dm = group->members[at];
            dm.parent.set(ev::setProperty(dm.parent.get(), dm.member, v.get()));
            return ev::undefined();
        },
        1));
    ev::deleteProperty(m.parent.get(), member);
    m.parent.set(ev::defineAccessor(m.parent.get(), member, getter.get(), setter.get(), /*enumerable=*/true));
    g.names.push_back(name);
    g.members.push_back(std::move(m));
}

void deferGlobal(ModuleGroup& g, const char* name) {
    ModuleGroup* gp = &g;
    g.names.emplace_back(name);
    ev::registerLazyGlobal(name, [gp] { runGroup(*gp); });
}

ModuleGroup& newGroup(const char* label, void (*install)()) {
    auto* g = new ModuleGroup();
    g->label = label;
    g->install = install;
    groups().push_back(g);
    return *g;
}

// bro.<sub>, or undefined.
Value broMember(const char* sub) {
    ev::GlobalValue broG = ev::globalValue("bro");
    if (!broG.found || !ev::isObject(broG.value)) return ev::undefined();
    return ev::getProperty(broG.value, sub);
}

}  // namespace

bool isDeferredName(const char* name) {
    for (const ModuleGroup* g : groups()) {
        if (g->started) continue;
        for (const std::string& n : g->names) {
            if (n == name) return true;
        }
    }
    return false;
}

void ensure3DModules() {
    if (g3d) runGroup(*g3d);
}

std::string deferredModuleGroups() {
    std::string out;
    for (const ModuleGroup* g : groups()) {
        if (g->started) continue;
        if (!out.empty()) out += ' ';
        out += g->label;
    }
    return out;
}

std::string loadedModuleGroups() {
    std::string out;
    for (const ModuleGroup* g : groups()) {
        if (!g->started) continue;
        if (!out.empty()) out += ' ';
        out += g->label;
    }
    return out;
}

void installDeferredModules() {
    // bro.image.gpu (js/image_gpu.js) on the `bro.image` the roots built.
    {
        ModuleGroup& g = newGroup("image.gpu", &installImageGpuModule);
        deferGlobal(g, "__bro_image_gpu");
        const ev::Persistent image(broMember("image"));
        deferMember(g, image.get(), "gpu");
    }
#if BRO_WITH_VISION
    // bro.vision.wildlife (js/vision_wildlife.js) on the `bro.vision` the
    // roots built.
    {
        ModuleGroup& g = newGroup("vision.wildlife", &installVisionWildlifeModule);
        deferGlobal(g, "__bro_vision_wildlife");
        const ev::Persistent vision(broMember("vision"));
        deferMember(g, vision.get(), "wildlife");
    }
#endif
#if BRO_WITH_3D
    // Every module of the family, in the boot order: each lists the classes an
    // earlier one lifted in js/module.globals.
    {
        ModuleGroup& g = newGroup("3d", [] {
            installPhysicsModule();
            installTerrainModule();
            installClipmapModule();
            installTileWorldModule();
            installLightingModule();
            installGizmoModule();
            installAnimationModule();
            installSceneModule();
            installImpostorModule();
        });
        g3d = &g;
        for (const char* name : {"Physics", "PhysicsCharacter", "PhysicsSoftBody", "PhysicsVehicle",
                                 "PhysicsRagdoll", "PhysicsWorldHandle", "Terrain", "ClipmapTerrain",
                                 "TileWorld", "Tween", "AnimationPlayer", "SceneNode", "SceneGraph"}) {
            deferGlobal(g, name);
        }
        const ev::Persistent bro(ev::globalValue("bro").value);
        for (const char* ns : {"scene", "terrain", "clipmap", "tile_world", "lighting", "animation",
                               "gizmo", "impostor"}) {
            deferMember(g, bro.get(), ns);
        }
    }
#endif
#if BRO_WITH_NET
    // The sync factory before js/net.js, which mounts `bro.net.sync` from
    // `globalThis.__bro_net_sync` at its own load.
    {
        ModuleGroup& g = newGroup("net", [] {
            installNetSyncModule();
            installNetModule();
        });
        deferGlobal(g, "__bro_net_sync");
        const ev::Persistent bro(ev::globalValue("bro").value);
        deferMember(g, bro.get(), "net");
    }
#endif
#if BRO_WITH_DIFFUSION && BRO_WITH_LM
    {
        ModuleGroup& g = newGroup("motion", &installMotionModule);
        const ev::Persistent bro(ev::globalValue("bro").value);
        deferMember(g, bro.get(), "motion");
    }
#endif
}

}  // namespace bro::bronze_host
