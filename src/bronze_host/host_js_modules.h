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
// checkpoint, in each compartment whose module registered its callback.
void fireHostObserverFrame();
// A compartment's watches, queued records and callbacks, at its teardown.
void dropObserverState(uint64_t compartmentId);

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
// js/clipboard.js: ClipboardItem and navigator.clipboard.write/read.
void installClipboardModule();
// js/notification.js: the web Notification API over bro.window.notify.
void installNotificationModule();
// The global object's prototype chain (host_global_proto.cpp): globalThis ->
// <ctorName>.prototype -> EventTarget.prototype (Object.prototype without
// one), with Symbol.toStringTag = <ctorName>, and <ctorName> registered as
// a global whose `new` is an Illegal constructor TypeError. Without it the
// global object has no prototype and String(window) throws. "Window" for the
// page realm, "DedicatedWorkerGlobalScope" for a Worker's.
void installGlobalPrototype(const char* ctorName);
void installNetSyncModule();
void installImageGpuModule();
// js/vision_wildlife.js: `__bro_vision_wildlife` mounted as
// bro.vision.wildlife. Main realm and Worker realms, after the sibling APIs
// built `bro.vision`; BRO_WITH_VISION builds only.
void installVisionWildlifeModule();
// js/bro_core.js: the public bro.time / bro.window / bro.settings /
// bro.appDir surface and the panels' __bro.*, assembled over the natives
// under __bro_native. Entered by installBroRoots (host_bro_root.cpp) after
// the roots and natives are registered; lifts nothing.
void installBroCoreModule();
// The 3D family (js/physics.js, terrain, tile_world, lighting,
// gizmo, animation, scene) and js/net.js / js/motion.js /
// js/server.js: each enters its module after installBroRoots and lifts the
// classes it defined. The sibling libraries' own APIs (bro.mesh, bro.lm,
// bro.stt, ...) are NOT here: installSiblingApis (host_natives.h) installs
// each exactly once per realm.
void installNetModule();
void installServerModule();
void installPhysicsModule();
void installTerrainModule();
void installTileWorldModule();
void installLightingModule();
void installGizmoModule();
void installAnimationModule();
void installSceneModule();
void installMotionModule();

// The main realm's wrapper modules that most apps never touch, deferred
// until a program first does: the 3D family (physics ... scene), net
// (net_sync + net), motion, bro.image.gpu and bro.vision.wildlife. Each group
// registers its globals as lazy host globals (bronze registerLazyGlobal: the
// manifest lists them, the first read runs the group) and turns its `bro.<ns>`
// members into accessors that run the group and then read back the member it
// mounted. Worker realms still enter their modules eagerly
// (installWorkerBroRoot).
void installDeferredModules();
// Run the 3D group now: a SceneGraph made by getContext('scene') needs the
// prototypes js/scene*.js install.
void ensure3DModules();
// A compartment's deferred groups, at its teardown.
void dropModuleGroups(uint64_t compartmentId);
// Whether `name` (a `bro.<ns>` member or a global) belongs to a group that
// has not run: markAvailableNamespaces skips it rather than run the group.
bool isDeferredName(const char* name);

}  // namespace bro::bronze_host
