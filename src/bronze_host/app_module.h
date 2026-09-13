#pragma once

// The compiled half of an app directory.
//
// An app dir holds a page (index.html) and, when its logic was compiled
// ahead of time, a shared module beside it — `app.dll` / `app.so` /
// `app.dylib`. This is how bro finds that module, decides whether it is safe
// to run, and runs its top level. `bro <folder>` is the caller: the same
// binary opens a script-based app and an AOT compiled one, and which it got is a
// property of the FOLDER rather than of the executable.
//
// That is the whole reason this file exists. The earlier arrangement linked
// each compiled app into its own executable at bro's configure time, which
// made bro's build system enumerate applications — a runtime's build has no
// business knowing what apps exist — and meant "another app" was another
// build of bro. Here the app is data the folder carries, and bro is one
// binary.
//
// Loading, not linking, moves two guarantees from link time to load time,
// and both are checked here rather than assumed:
//
//   * the ABI. bronze stamps every emitted object with the hash of the
//     bronze_abi.h it was compiled against; the runtime carries its own.
//     A stale LINKED object at least forced a relink; a stale MODULE loads
//     happily and then reads arguments that were never passed — not a crash,
//     but nondeterministic stalls (the failure bronze_abi.h names as its
//     motivation). So the stamp is compared BEFORE any compiled code runs,
//     and a mismatch refuses the module.
//
//   * the entry point. A module with no `bronze_main` is a file that is not
//     a compiled app, and saying so beats calling a null pointer.
//
// Split in two on purpose. `findAppModule` answers a question about a
// directory and must run BEFORE the Engine is constructed, because
// EngineConfig::hostProvidesCompiledApp is what lets engine init tell an app
// that forgot `"compiled": true` from one that declared it and was opened by
// a host with nothing to run (engine_init.cpp). `runAppModule` needs the
// Engine, because the host globals it installs are backed by it.

#include <cstdint>
#include <optional>
#include <string>

namespace bronze::embed {
using ModuleHandle = uint64_t;
}

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

using ModuleHandle = void*;

/// Load a shared library/module at `path`. On failure, sets `error` and returns nullptr.
ModuleHandle openModule(const std::string& path, std::string& error);

/// Look up an exported symbol by name from `handle`.
void* moduleSymbol(ModuleHandle handle, const char* name);

/// Path of the compiled module `appDir` carries, or nothing when it carries
/// none. Existence only — whether the file is loadable, matches this
/// runtime's ABI, or is a bronze module at all is `runAppModule`'s business.
/// The caller needs the answer before the Engine exists, and at that point
/// the only honest answer is "there is a file there".
std::optional<std::string> findAppModule(const std::string& appDir);

/// What became of a module `findAppModule` located.
enum class AppModuleStatus {
    Ran,           ///< loaded, ABI verified, top level ran to completion
    Unloadable,    ///< the OS refused to load it (missing dependency, wrong arch)
    Unstamped,     ///< loaded, but carries no bronze ABI stamp: not a bronze module
    AbiMismatch,   ///< a bronze module, compiled against a different ABI
    NoEntryPoint,  ///< stamped, but exports no `bronze_main`
};

struct AppModuleResult {
    AppModuleStatus status = AppModuleStatus::Unloadable;
    /// One sentence naming what is wrong and what to do about it, already
    /// written to the log. Carried back so a caller can also put it somewhere
    /// a user will see — bro.exe's log is a file, and a compiled app that
    /// refuses to start must not look like a hang.
    std::string detail;
    /// The Bronze GC module handle for this app module (0 if not loaded/ran).
    bronze::embed::ModuleHandle moduleHandle{0};
};

/// Load `modulePath`, verify it, install the web host globals on `engine`,
/// and run the compiled top level — the sequence
/// `bronze::embed::runMain()` performs for a LINKED object, with the entry
/// point and the ABI stamp resolved by symbol lookup instead.
///
/// Call after the Engine exists and before its run loop: the top level reads
/// the host globals as it runs and ends by scheduling work (typically a
/// requestAnimationFrame) that the frame loop then drives.
///
/// Module roots are bracketed via `bronze::embed::beginModuleLoad()` and can
/// be detached on app reload via `unloadAppModule()`. The underlying OS
/// shared library image is never dlclosed/FreeLibrary-freed because surviving
/// heap function objects may hold code pointers into its .text.
AppModuleResult runAppModule(engine::Engine& engine, const std::string& modulePath);

/// Unload a bronze module handle previously returned from beginModuleLoad().
/// Removes the module's root spans from Bronze GC tracking.
void unloadAppModule(bronze::embed::ModuleHandle handle);

/// Whether `status` means the app is running. Anything else left the page
/// scripts-only, with `detail` saying why.
inline bool ran(AppModuleStatus status) { return status == AppModuleStatus::Ran; }

}  // namespace bro::bronze_host
