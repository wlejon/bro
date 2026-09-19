#pragma once

// The `bro` / `__bro` roots and the natives beneath them — the first surface
// rebuilt on bronze's native mechanism (embed::registerNative) rather than on
// hand marshalling.
//
// THE CONVENTION, applied to every namespace here and meant for the ones that
// mount later:
//
//   * `bro` and `__bro` are HOST GLOBALS holding plain objects, and every
//     namespace under them (`bro.time`, `__bro.perf`) is a plain object too.
//     Nothing on them is native: the public surface is JavaScript, assembled
//     by js/bro_core.js, so `const t = bro.time; t.scale` and `Object.keys`
//     and every other dynamic access work exactly as a program expects.
//
//   * The natives all live under ONE internal root, `__bro_native`, also a
//     host global holding a plain object with one sub-object per namespace.
//     `__bro_native.time.scale` is a getter/setter pair, `__bro_native.
//     settings.get` a function, and so on. bro_core.js names each by its
//     full dotted path, which is what the compiler lowers to a direct call —
//     an alias (`const N = __bro_native`) would NOT be, because the lowering
//     only short-circuits a free identifier's dotted path.
//
//   * Compound results are never a struct type: a fixed shape is exposed as
//     its scalar pieces and assembled in the wrapper (a display is fourteen
//     natives over a snapshot; a window position is two), and a genuinely
//     dynamic value crosses as JSON text (`str`) the wrapper parses. Compound
//     ARGUMENTS follow the same rule in reverse: a list of binding strings
//     crosses as one newline-joined `str` (a binding never contains a
//     newline).
//
//   * A callback is a `dynamic` parameter the C side keeps in a Persistent
//     and invokes through embed::call from the engine's own hook point.
//
//   * A NATIVE CLASS (`__bro_native.mesh.Mesh`, native_mesh.cpp) is a
//     registered constructor whose handle owns the C++ object, and its
//     members are namespace FUNCTIONS taking the handle as a class-typed
//     first parameter rather than native methods: the compiler lowers a
//     method to a direct call only where it can see the receiver's class,
//     and inside a wrapper's accessor the receiver is `this`, which it
//     cannot. The PUBLIC class (js/mesh.js's `Mesh`) is a JavaScript class
//     whose constructor returns the native handle (`return new
//     __bro_native.mesh.Mesh(...)`), with the native prototype — published
//     by C++ as `__bro_native.mesh.MeshPrototype` from
//     embed::nativeClassPrototype — chained under the public prototype by
//     Object.setPrototypeOf. So every handle IS an instance (`instanceof
//     Mesh`, prototype methods, getters) and there is one object, not a
//     wrapper around a handle. Exposing the native constructor directly was
//     not an option: a native is not a property of `__bro_native.mesh`, so
//     a dynamic `bro.mesh.Mesh` would read undefined.
//
//   * A typed-array RESULT (`f32[]`, `u32[]`, `u8[]`) fills the trailing
//     bronze_native_buffer in one of two modes: COPY (release null) for
//     storage the C++ side keeps — a mesh's attribute vectors, a per-thread
//     scratch — and TRANSFER (release set) for a fresh result nobody else
//     holds, which the program's array views in place until it is
//     collected. native_mesh.cpp says which each of its natives uses and why.
//
// WHY THE NATIVES ARE NOT AT THE PUBLIC PATHS. A native registered as
// `bro.time.scale` is reached only by a compiled `bro.time.scale` spelled in
// full; every other access lands on the plain `bro.time` object, which would
// then need a second, hand-marshalled copy of the same member to answer.
// One internal root and one JavaScript wrapper is one definition per member.
//
// The manifest a build compiles bro_core.js against is written by
// bro-native-manifest (native_manifest_tool.cpp), which links this layer and
// calls registerBroNatives — the same registrations bro makes at run time,
// so the two cannot drift.

#include <functional>
#include <initializer_list>
#include <string>

#include "runtime/value.h"

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

// Register every native under `__bro_native` on the calling thread. False
// with `*error` set at the first refusal. No engine is needed: registration
// records C entry points and signatures, and the entry points reach the
// engine through hostEngine() when called.
bool registerBroNatives(std::string* error);

// The per-namespace halves of registerBroNatives, one per native_*.cpp.
bool registerTimeNatives(std::string* error);
// bro.server (native_server.cpp): the engine's server loop on the main
// thread (bro-server's tick rate, uptime and stop), and on a Worker's thread
// the worker's own loop through the control the worker set with
// setWorkerServerControl before its script ran.
bool registerServerNatives(std::string* error);
struct WorkerServerControl {
    std::function<double()> tickRate;
    std::function<void(double)> setTickRate;
    std::function<double()> uptimeSec;
    std::function<void()> stop;
};
// Per thread; null (the default) means "this thread's realm is the engine's".
void setWorkerServerControl(const WorkerServerControl* control);
bool registerWindowNatives(std::string* error);
bool registerSettingsNatives(std::string* error);
bool registerPathsNatives(std::string* error);
bool registerDunderBroNatives(std::string* error);
inline bool registerMeshNatives(std::string*) { return true; }
bool registerNetNatives(std::string* error);
inline bool registerRiggingNatives(std::string*) { return true; }
// The physics natives (native_physics_*.cpp, natives/physics/) are in the
// library only with BRO_WITH_PHYSICS (CMakeLists.txt); without it there is
// nothing to register and js/physics.js is not entered.
#if BRO_WITH_PHYSICS
bool registerPhysicsNatives(std::string* error);
#else
inline bool registerPhysicsNatives(std::string*) { return true; }
#endif
bool registerAnimationNatives(std::string* error);
bool registerTerrainNatives(std::string* error);
bool registerClipmapNatives(std::string* error);
bool registerTileWorldNatives(std::string* error);
bool registerLightingNatives(std::string* error);
bool registerGizmoNatives(std::string* error);
bool registerSceneNatives(std::string* error);
inline bool registerLmNatives(std::string*) { return true; }
inline bool registerRaveNatives(std::string*) { return true; }
bool registerMotionNatives(std::string* error);
inline bool registerMicNatives(std::string*) { return true; }
inline bool registerSenseNatives(std::string*) { return true; }
inline bool registerGestureNatives(std::string*) { return true; }
inline bool registerWakeNatives(std::string*) { return true; }
inline bool registerKwsNatives(std::string*) { return true; }
inline bool registerListenNatives(std::string*) { return true; }
inline bool registerTriposplatNatives(std::string*) { return true; }
inline bool registerDiffusionNatives(std::string*) { return true; }
inline bool registerVisionNatives(std::string*) { return true; }
inline bool registerDiarNatives(std::string*) { return true; }
inline bool registerSttNatives(std::string*) { return true; }
inline bool registerTtsNatives(std::string*) { return true; }
inline bool registerFloraNatives(std::string*) { return true; }
inline bool registerTensorNatives(std::string*) { return true; }
// bro.net's state is per realm (one NetSubscriber and one dispatcher per
// thread, native_net.cpp): pollNet drains the CALLING thread's subscriber
// and fires its callbacks there; releaseNetState hands the calling thread's
// subscriber back to the service and frees the state, which a Worker does
// as its realm ends. Both are no-ops on a thread that never touched bro.net.
void pollNet();
void releaseNetState();

inline void publishMeshPrototypes(bronze::Value) {}
inline void publishRiggingPrototypes(bronze::Value) {}

// Register the roots (`bro`, `__bro`, `__bro_native` with their namespace
// objects), the natives, the engine-side hooks the callbacks ride on, and
// run js/bro_core.js on top. Called once from installWebHostGlobals.
void installBroRoots(engine::Engine& engine);

// A Worker realm's `bro`: the root object with `bro.net` (and `bro.net.sync`)
// over the net natives registered on the worker's thread, the compute
// namespaces a worker is FOR (bro.mesh / bro.rigging and their classes,
// bro.math, bro.image, bro.flora, bro.ai.game, bro.gpu / bro.tensor, bro.lm,
// bro.stt / tts / diar / rave, bro.diffusion / triposplat, bro.vision,
// bro.motion, bro.media) and a worker-scoped bro.server — docs/worker-api.js.
// No engine-bound namespace (window, settings, time, the scene, audio),
// since none of those is safe off the main thread. Called by
// WorkerInstance's thread after brokit is installed and before the worker
// script runs.
void installWorkerBroRoot();

// `available: true` on every compiled-in feature namespace under `bro` (and
// on `Physics`), after all wrapper modules have mounted; the compiled-out
// stubs already answer false. Called last from installWebHostGlobals and
// installWorkerBroRoot.
void markAvailableNamespaces();

// Every sibling library's JS API (broaudio, brogameagent, bromesh,
// brotensor, brolm, brosoundml, brodiffusion, brovisionml, broflora,
// broimage), each installed exactly once PER REALM, onto the roots
// installBroRoots has just published. host_sibling_apis.cpp is the ONLY
// place in bro that calls a sibling's install*(); none of them survives a
// second call on the same thread.
void installSiblingApis(engine::Engine& engine);

// The Worker realm's share of that list: the siblings whose state is per
// thread (their HostClass constructors and install guards, bronze's native
// registry), installed on the calling worker's thread over its own roots.
// Called from installWorkerBroRoot; tickWorkerSiblingApis pumps the calling
// thread's async jobs from the worker's loop.
void installWorkerSiblingApis();
void tickWorkerSiblingApis();

// The engine hook bro.settings.onChange listens through (native_settings.cpp).
void installSettingsObserver(engine::Engine& engine);

// ---- registration helpers, shared by the native_*.cpp files ---------------

namespace natives {

// A function: `path(params) -> ret`.
bool fn(const char* path, void* f, const char* ret,
        std::initializer_list<const char*> params, std::string* error);
// A namespace property's read half: `path -> ret`.
bool getter(const char* path, void* f, const char* ret, std::string* error);
// A namespace property's write half: `path = <type>`.
bool setter(const char* path, void* f, const char* type, std::string* error);
// A class: `new path(params)` calls `f`, which answers the void* the handle
// owns; `dtor` (may be null) runs on that pointer when the handle dies.
// Registers the class, so any native naming `path` as a type comes after.
bool ctor(const char* path, void* f, void (*dtor)(void*),
          std::initializer_list<const char*> params, std::string* error);

// A `str` result must outlive the return: the runtime copies it during the
// call. This hands back a per-thread scratch the next str-returning native
// may reuse.
const char* strResult(std::string s);

}  // namespace natives

}  // namespace bro::bronze_host
