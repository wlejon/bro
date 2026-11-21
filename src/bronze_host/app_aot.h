#pragma once

// An installed app's page scripts, compiled ahead of time (docs/apps.md,
// "Ahead-of-time scripts").
//
// `bro --install` / `bro --build-exe` builds an app's executable (engine/
// app_exe.h) and then runs `bro-headless <app> --aot-build <dir>`: a headless
// engine for the app sets up the page's realm exactly as a launch does — host
// globals, natives, module roots — and, instead of running the page's script
// units (Engine::pageScriptUnits), compiles each one that has a PROFILE
// through bronze's evaluator to a loadable module built on it (bronze eval.h,
// buildModule). The modules are written by brass's own image writer, so the
// user's machine needs no compiler or linker. Each unit's module sits in the
// executable's AOT directory (appExeAotDir) beside a record of everything its
// compile read.
//
// A unit's profile is its first run: with no module, the unit compiles in
// process TO BE HARVESTED (EvalOptions::harvestOutPath: lowered as a module
// build lowers, counting in every tier what such a build counts), the
// harvest is written as its profile when the page is hidden or the app quits,
// and the directory is rebuilt in the background then (settleAotHarvests), so
// the second launch runs the module. There is no module built on nothing. A
// module whose run enters often a function its profile saw seldom or never
// (compiled cold) was built on too thin a run: its profile and record are
// dropped, and the next launch is harvested again.
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
/// it without a table. `ordinal` tells apart units of one name and kind, in
/// page order: a page's classic scripts and each of its inline module scripts
/// are all named for the page, and shared one record until they had it (the
/// later build overwrote the earlier, which then ran as out of date).
std::string aotUnitId(const std::string& filename, bool publishEntry, int ordinal = 0);

/// A page's units are about to run, first to last (the load and each reload):
/// the ordinals takeAotUnit counts start over.
void beginAotUnitRuns();

/// What `bro --exe-info` reports of an AOT directory (`aot.txt`, written by
/// buildAppAot): `aot.<key>=<value>` lines, "" when there is none.
std::string describeAppAot(const std::string& dir);

/// `bro-headless --aot-build <dir>`: compile `units` that have a profile into
/// `dir` against this engine's realm, keeping a unit whose module is already
/// current; one without waits for its first run's harvest. False when a unit
/// failed to compile (it runs on the JIT; aot.txt says why).
bool buildAppAot(engine::Engine& engine, const std::vector<engine::Engine::PageScriptUnit>& units,
                 const std::string& dir);

/// The unit's module, loaded, when `engine` runs from an AOT directory that
/// holds a current one for it (`execCode` and `opts` are exactly what the
/// unit would be compiled from); nothing otherwise. A `pageUnit` with no
/// module (none yet, or one out of date, logged) gets `opts.harvestOutPath`:
/// it compiles in process to be harvested. One whose profile awaits its
/// build compiles as it is and asks for the build, once per process.
std::optional<bronze::eval::LoadedModule> loadAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      bronze::eval::EvalOptions& opts, int ordinal, bool pageUnit);

/// The page is hidden or the app quits, its harvests just written
/// (Engine::firePageHide): each unit this process harvested has its profile,
/// and the directory is rebuilt in the background. A module run here that
/// entered a function it compiled cold more than about once a frame loses
/// its profile and record, for its next launch to be harvested again.
void settleAotHarvests(engine::Engine& engine);

/// At launch, in place of the page's first unit's ahead compile
/// (startAppScriptCompile): its module, loaded now when current and kept for
/// the run of exactly that unit; the page's compile is then timed with
/// `codeCache` 'aot' (bro.app.startup.page). True when one was kept.
bool loadAheadAotUnit(engine::Engine& engine, const std::string& execCode, bronze::eval::EvalOptions& opts);

/// A unit's run: the module loadAheadAotUnit kept for it, else loadAotUnit's.
std::optional<bronze::eval::LoadedModule> takeAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      bronze::eval::EvalOptions& opts, bool pageUnit);

/// Drops a module loadAheadAotUnit kept that no run took.
void discardAheadAotUnit();

// A dedicated Worker's script is a unit of its own, with the same cycle on
// the worker's thread: its first run compiles to be harvested, the harvest is
// written as the worker ends (bronze's harvest is per thread), and the build
// makes the unit's module on it in a worker realm set up as the worker's run
// sets it up (WorkerAotBuild). A sidecar `<id>.worker` beside the record says
// how to make that realm: the script's path, its base path, classic or
// module. A worker made from a blob: or data: URL has no file to be the unit
// of and always compiles.
struct WorkerAotRun;

/// The worker script's module (`code` and `opts` exactly what it would be
/// compiled from) when `engine` runs from an AOT directory holding a current
/// one; nothing otherwise, and then `opts.harvestOutPath` when the run is to
/// be harvested. Worker thread.
std::optional<bronze::eval::LoadedModule> loadWorkerAotUnit(engine::Engine& engine, const std::string& code,
                                                            bronze::eval::EvalOptions& opts, bool moduleScript,
                                                            const std::string& basePath, WorkerAotRun& run);

/// The worker is ending, its harvest (if any) just written on its thread:
/// the harvest becomes the unit's profile, and a module whose run entered a
/// cold function about once a frame or more loses its profile and record.
/// The directory's rebuild waits for the last harvesting worker. Worker thread.
void settleWorkerAot(WorkerAotRun& run);

}  // namespace bro::bronze_host
