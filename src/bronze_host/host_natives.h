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

#include <initializer_list>
#include <string>

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

// Register every native under `__bro_native` on the calling thread. False
// with `*error` set at the first refusal. No engine is needed: registration
// records C entry points and signatures, and the entry points reach the
// engine through hostEngine() when called.
bool registerBroNatives(std::string* error);

// The per-namespace halves of registerBroNatives, one per native_*.cpp.
bool registerTimeNatives(std::string* error);
bool registerWindowNatives(std::string* error);
bool registerSettingsNatives(std::string* error);
bool registerPathsNatives(std::string* error);
bool registerDunderBroNatives(std::string* error);

// Register the roots (`bro`, `__bro`, `__bro_native` with their namespace
// objects), the natives, the engine-side hooks the callbacks ride on, and
// run js/bro_core.js on top. Called once from installWebHostGlobals.
void installBroRoots(engine::Engine& engine);

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

// A `str` result must outlive the return: the runtime copies it during the
// call. This hands back a per-thread scratch the next str-returning native
// may reuse.
const char* strResult(std::string s);

}  // namespace natives

}  // namespace bro::bronze_host
