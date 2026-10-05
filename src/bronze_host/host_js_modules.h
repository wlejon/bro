#pragma once

// bro's own bronze-compiled JavaScript (host_js_modules.cpp), and the host
// hooks the observer module runs over (host_observer_hooks.cpp).

#include "embed/embed.h"

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// `__bro_observers`: the mutation-record take and the frame tick that
// js/observers.js builds MutationObserver, ResizeObserver and
// IntersectionObserver on. Registered by installObserversModule before the
// module's entry runs.
void installObserverHooks();

// The observer module's per-frame pass (resize, then intersection), fired
// from the frame seam after requestAnimationFrame and its microtask
// checkpoint. A no-op until the module has registered its callback.
void fireHostObserverFrame();

// Lift a value some compiled JS (or a sibling installer) ASSIGNED onto
// `globalThis` into bronze's host-global registry, so a compiled read of the
// bare name answers with the same object. A no-op for a name globalThis
// lacks.
void adoptGlobalProperty(const char* name);

// Enter each compiled module and lift what it defined on globalThis into the
// host-global registry: MutationObserver/ResizeObserver/IntersectionObserver
// and their entry classes; the UI event classes (MouseEvent, KeyboardEvent,
// ...) over brokit's Event; `__bro_net_sync` (a factory over the bro.net
// primitives); `__bro_image_gpu` (colormap, fbm2D).
void installObserversModule();
void installEventsModule();
// The global object's prototype chain (host_global_proto.cpp): globalThis ->
// <ctorName>.prototype -> EventTarget.prototype (Object.prototype without
// one), with Symbol.toStringTag = <ctorName>, and <ctorName> registered as
// a global whose `new` is an Illegal constructor TypeError. Without it the
// global object has no prototype and String(window) throws. "Window" for the
// page realm, "DedicatedWorkerGlobalScope" for a Worker's.
void installGlobalPrototype(const char* ctorName);
void installNetSyncModule();
void installImageGpuModule();
// js/bro_core.js: the public bro.time / bro.window / bro.settings /
// bro.appDir surface and the panels' __bro.*, assembled over the natives
// under __bro_native. Entered by installBroRoots (host_bro_root.cpp) after
// the roots and natives are registered; lifts nothing.
void installBroCoreModule();
// The 3D family (js/physics.js, terrain, clipmap, tile_world, lighting,
// gizmo, animation, scene, impostor) and js/net.js / js/motion.js /
// js/server.js: each enters its module after installBroRoots and lifts the
// classes it defined. The sibling libraries' own APIs (bro.mesh, bro.lm,
// bro.stt, ...) are NOT here: installSiblingApis (host_natives.h) installs
// each exactly once per realm.
void installNetModule();
void installServerModule();
void installPhysicsModule();
void installTerrainModule();
void installClipmapModule();
void installTileWorldModule();
void installLightingModule();
void installGizmoModule();
void installAnimationModule();
void installSceneModule();
void installMotionModule();
void installImpostorModule();

}  // namespace bro::bronze_host
