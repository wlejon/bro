#pragma once

// An installed app's page scripts, compiled ahead of time (docs/apps.md,
// "Ahead-of-time scripts").
//
// `bro --install` / `bro --build-exe` builds an app's executable (engine/
// app_exe.h) and then runs `bro-headless <app> --aot-build <dir>`: a headless
// engine for the app sets up the page's realm exactly as a launch does — host
// globals, natives, module roots — and, instead of running the page's script
// units (Engine::pageScriptUnits), compiles each one through bronze's
// evaluator to a loadable module (bronze eval.h, buildModule). The modules are
// written by brass's own image writer, so the user's machine needs no
// compiler or linker. Each unit's module sits in the executable's AOT
// directory (appExeAotDir) beside a record of everything its compile read.
//
// The executable runs with that directory as EngineConfig::aotDir. Each unit
// the page runs asks loadAotUnit for its module first: bronze checks the
// record against the unit's text and options as they are now (the same key
// the code cache uses, every source file's digest, module resolution, the
// runtime ABI), so a module runs only while it is still the program the JIT
// would compile. Anything else — an edited file, another engine, another
// host-global list — compiles the unit in process as a folder launch does, and
// asks for the directory to be rebuilt behind it.
//
// The JIT stays: eval, new Function and import() compile in process from
// inside AOT code, and a plain `bro <folder>` (the edit loop, hot reload)
// never looks at an AOT directory.

#include "engine/engine.h"

#include "eval/eval.h"

#include <optional>
#include <string>
#include <vector>

namespace bro::bronze_host {

/// The name a unit's record goes by in an AOT directory (`<id>.bzm`): a
/// digest of the unit's compile name and whether it is published as a module
/// file (EvalOptions filename / publishEntry), so the build and a run agree on
/// it without a table.
std::string aotUnitId(const std::string& filename, bool publishEntry);

/// What `bro --exe-info` reports of an AOT directory (`aot.txt`, written by
/// buildAppAot): `aot.<key>=<value>` lines, "" when there is none.
std::string describeAppAot(const std::string& dir);

/// `bro-headless --aot-build <dir>`: compile `units` into `dir` against this
/// engine's realm, keeping a unit whose module is already current. False when
/// a unit failed to compile (it runs on the JIT; aot.txt says why).
bool buildAppAot(engine::Engine& engine, const std::vector<engine::Engine::PageScriptUnit>& units,
                 const std::string& dir);

/// The unit's module, loaded, when `engine` runs from an AOT directory that
/// holds a current one for it (`execCode` and `opts` are exactly what the
/// unit would be compiled from); nothing otherwise. A stale record is logged
/// and starts a rebuild of the directory, once per process.
std::optional<bronze::eval::LoadedModule> loadAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      const bronze::eval::EvalOptions& opts);

/// At launch, in place of the page's first unit's ahead compile
/// (startAppScriptCompile): its module, loaded now when current and kept for
/// the run of exactly that unit; the page's compile is then timed with
/// `codeCache` 'aot' (bro.app.startup.page). True when one was kept.
bool loadAheadAotUnit(engine::Engine& engine, const std::string& execCode, const bronze::eval::EvalOptions& opts);

/// A unit's run: the module loadAheadAotUnit kept for it, else loadAotUnit's.
std::optional<bronze::eval::LoadedModule> takeAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      const bronze::eval::EvalOptions& opts);

/// Drops a module loadAheadAotUnit kept that no run took.
void discardAheadAotUnit();

}  // namespace bro::bronze_host
