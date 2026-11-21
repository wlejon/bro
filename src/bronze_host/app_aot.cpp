// An installed app's page scripts compiled ahead of time: the build inside
// `bro-headless --aot-build`, and the lookup each unit makes at launch.
// app_aot.h holds the reasoning.

#include "bronze_host/app_aot.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/host_gc.h"
#include "bronze_host/host_worker_internal.h"
#include "engine/app_exe.h"
#include "engine/app_runtime.h"
#include "util/log.h"
#include "util/process_spawn.h"
#include "util/sha.h"
#include "util/time.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

namespace fs = std::filesystem;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string u8str(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

#ifdef _WIN32
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModuleExt = ".so";
#endif

constexpr const char* kSummaryFile = "aot.txt";

fs::path recordPath(const std::string& dir, const std::string& id) { return u8path(dir) / (id + ".bzm"); }
// The unit's type feedback: what its first run's inline caches and counters
// held when the run ended (bronze docs/type-feedback.md, "Harvest"), which
// the unit's module is built on. A unit has no module without one.
fs::path profilePath(const std::string& dir, const std::string& id) { return u8path(dir) / (id + ".tfb"); }

// A module's profile was too thin a run when a later run enters a function it
// compiled cold this often: kColdEntriesPerSecond a second of the run (about a
// call a frame), and never fewer than kColdEntriesMin. Its unit is then
// harvested again (settleAotHarvests): a launch closed after two seconds holds
// the start-up alone, and the module built on it would otherwise keep
// everything the app does after start-up cold for as long as it is current.
constexpr double kColdEntriesPerSecond = 64.0;
constexpr uint64_t kColdEntriesMin = 4096;

// A fresh module name for each build of a unit: a running executable keeps
// the module it loaded mapped, and on Windows a mapped image cannot be
// replaced, only left behind.
fs::path freshModulePath(const std::string& dir, const std::string& id) {
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    return u8path(dir) / (id + "-" + std::to_string(static_cast<long long>(stamp)) + kModuleExt);
}

// A worker unit's sidecar: how the build makes the realm its run has
// (app_aot.h, "A dedicated Worker's script").
constexpr const char* kWorkerSidecarExt = ".worker";
fs::path workerSidecarPath(const std::string& dir, const std::string& id) {
    return u8path(dir) / (id + kWorkerSidecarExt);
}

// A worker unit is named by its script's path and kind, apart from the page's
// units of the same file.
std::string workerUnitId(const std::string& filename, bool moduleScript) {
    util::Sha256 h;
    const std::string material = std::string(moduleScript ? "worker module\n" : "worker script\n") + filename;
    h.update(material.data(), material.size());
    return h.hex().substr(0, 24);
}

struct WorkerSidecar {
    std::string script;  // the worker's script path as the run resolved it
    std::string base;    // the WorkerInstance base path it was made with
    bool moduleScript = false;
};

bool readWorkerSidecar(const fs::path& path, WorkerSidecar& out) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    while (in && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("script=", 0) == 0) out.script = line.substr(7);
        else if (line.rfind("base=", 0) == 0) out.base = line.substr(5);
        else if (line == "module=1") out.moduleScript = true;
    }
    return !out.script.empty();
}

// Whether a module built on a profile was built on too thin a run: it entered
// a function compiled cold about once a frame or more (kColdEntriesPerSecond).
bool profileTooThin(const uint64_t* coldEntries, uint32_t count, double startMs, uint64_t& most,
                    double& seconds) {
    most = 0;
    for (uint32_t i = 0; i < count; ++i) most = std::max(most, coldEntries[i]);
    seconds = std::max(0.0, util::currentTimeMs() - startMs) / 1000.0;
    const uint64_t limit = std::max(kColdEntriesMin, static_cast<uint64_t>(seconds * kColdEntriesPerSecond));
    return most >= limit;
}

// One line: a reason or a path never carries a newline into aot.txt.
std::string oneLine(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    if (s.size() > 400) s.resize(400);
    return s;
}

}  // namespace

std::string aotUnitId(const std::string& filename, bool publishEntry, int ordinal) {
    util::Sha256 h;
    // The first unit of a name keeps the id it always had.
    std::string material = std::string(publishEntry ? "module\n" : "script\n") + filename;
    if (ordinal > 0) material += "\n#" + std::to_string(ordinal);
    h.update(material.data(), material.size());
    return h.hex().substr(0, 24);
}

std::string describeAppAot(const std::string& dir) {
    std::ifstream in(u8path(dir) / kSummaryFile, std::ios::binary);
    if (!in) return {};
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out += "aot." + line + "\n";
    }
    return out;
}

bool buildAppAot(engine::Engine& engine, const std::vector<engine::Engine::PageScriptUnit>& units,
                 const std::string& dir) {
    HostEvalScope evalScope;
    if (!isWebHostGlobalsInstalled()) installWebHostGlobals(engine);
    std::error_code ec;
    fs::create_directories(u8path(dir), ec);

    // Modules another engine build wrote are compiled again, current record
    // or not. A record only says the module is still the program this text
    // compiles to against this ABI, which stays true across compiler builds;
    // what the newer compiler would make of it (a fix, a faster body) reaches
    // an installed app only by building it again, and a new engine is when
    // its executable is rebuilt anyway.
    bool otherEngine = true;
    {
        std::ifstream prior(u8path(dir) / kSummaryFile, std::ios::binary);
        std::string line;
        if (prior && std::getline(prior, line)) otherEngine = line != "engine=" + engine::engineVersion();
    }

    // The registry each later unit is compiled against: what the realm holds
    // now, plus every module file the units before it publish when they run.
    // A unit that turns out to run against another registry (one whose top
    // level ran an import()) finds its record stale and compiles in process.
    std::vector<std::string> registry;
    {
        bronze::eval::EvalOptions probe;
        probe.moduleRegistry = true;
        probe.hostGlobals = {"-"};  // the registry is all this capture is for
        probe.nativeManifestJson = std::string();
        bronze::eval::captureThreadInputs(probe);
        registry = probe.externalModules;
    }
    std::map<std::string, int> buildOrdinals;  // units of one name and kind, counted in page order
    std::set<std::string> keep;  // module files a record names
    std::string summary = "engine=" + engine::engineVersion() + "\n";
    summary += "units=" + std::to_string(units.size()) + "\n";
    bool allOk = true;
    int built = 0, current = 0, waiting = 0;
    for (size_t i = 0; i < units.size(); ++i) {
        const auto& unit = units[i];
        bronze::eval::EvalOptions opts = 
            scriptEvalOptions(engine, unit.name, unit.moduleFile, pageUnitGoal(unit.classicScripts));
        opts.externalModules = registry;
        opts.externalModulesCaptured = true;
        const std::string execCode = scriptExecCode(unit.code, unit.name);
        const std::string id =
            aotUnitId(opts.filename, opts.publishEntry, buildOrdinals[aotUnitId(opts.filename, opts.publishEntry)]++);
        const std::string record = u8str(recordPath(dir, id));
        const fs::path profile = profilePath(dir, id);
        std::string status;

        const auto start = std::chrono::steady_clock::now();
        const bool hasProfile = fs::is_regular_file(profile, ec);
        if (hasProfile) opts.feedbackInPath = u8str(profile);
        bronze::eval::LoadedModule existing;
        if (hasProfile && !otherEngine) existing = bronze::eval::loadModule(execCode, opts, record);
        if (!hasProfile) {
            // Nothing to build yet: the unit's first run compiles in process
            // and is harvested (loadAotUnit), and the build after it makes
            // the unit's module on what that run did.
            fs::remove(u8path(record), ec);
            status = "waiting for its profile";
            ++waiting;
        } else if (existing.ok) {
            keep.insert(u8str(u8path(existing.modulePath).filename()));
            status = "current";
            ++current;
        } else {
            const fs::path module = freshModulePath(dir, id);
            bronze::eval::ModuleBuildResult res = bronze::eval::buildModule(execCode, opts, u8str(module), record);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (res.success) {
                keep.insert(u8str(module.filename()));
                char buf[64];
                std::snprintf(buf, sizeof(buf), "built %.0f ms", ms);
                status = std::string(buf) + " on its profile, " + std::to_string(res.coldFunctions) + " of " +
                         std::to_string(res.functions) + " functions cold";
                ++built;
                for (const std::string& f : res.filesRead) {
                    if (std::find(registry.begin(), registry.end(), f) == registry.end()) registry.push_back(f);
                }
            } else if (res.profileStale) {
                // Another build's harvest (the unit's text, its inputs or the
                // compiler changed since): the next run is harvested again.
                fs::remove(profile, ec);
                fs::remove(u8path(record), ec);
                status = "stale profile dropped, waiting for its profile";
                ++waiting;
            } else {
                // The unit runs on the JIT; the reason is the build's to report.
                status = "failed: " + oneLine(res.errorMessage);
                allOk = false;
                fs::remove(module, ec);
                fs::remove(u8path(record), ec);
                LOG_ERROR("aot: %s did not compile ahead of time, it will compile at launch:\n%s", unit.name.c_str(),
                          res.errorMessage.c_str());
            }
        }
        if (unit.moduleFile) {
            // A module script publishes its own file when it runs, whether
            // its module was built now, kept, or failed and it runs on the
            // JIT, so the units after it are compiled against a registry
            // that holds it (a build's filesRead lists only what the entry
            // imported). A kept module's imports are not listed: only a
            // build reports them.
            std::error_code aec;
            const std::string entry = fs::absolute(u8path(unit.name), aec).generic_string();
            if (std::find(registry.begin(), registry.end(), entry) == registry.end()) registry.push_back(entry);
        }
        summary += "unit" + std::to_string(i) + "=" + id + " " + status + " " + oneLine(unit.name) + "\n";
        LOG_INFO("aot: %s: %s", unit.name.c_str(), status.c_str());
    }

    // The workers the app's runs made (their sidecars), each built in a worker
    // realm of its own, as its run has.
    int workerUnits = 0;
    std::vector<fs::path> sidecars;
    for (auto it = fs::directory_iterator(u8path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == kWorkerSidecarExt) sidecars.push_back(it->path());
    }
    std::sort(sidecars.begin(), sidecars.end());
    for (const fs::path& sidecar : sidecars) {
        const std::string id = u8str(sidecar.stem());
        const fs::path profile = profilePath(dir, id);
        const fs::path record = recordPath(dir, id);
        WorkerSidecar w;
        std::string status;
        if (!readWorkerSidecar(sidecar, w) || !fs::is_regular_file(u8path(w.script), ec) ||
            workerUnitId(w.script, w.moduleScript) != id) {
            // The script is gone (or the sidecar is not one): so is its unit.
            fs::remove(sidecar, ec);
            fs::remove(profile, ec);
            fs::remove(record, ec);
            continue;
        }
        if (!fs::is_regular_file(profile, ec)) {
            fs::remove(record, ec);
            status = "waiting for its profile";
            ++waiting;
        } else {
            const auto start = std::chrono::steady_clock::now();
            const fs::path module = freshModulePath(dir, id);
            WorkerAotBuild build;
            build.modulePath = u8str(module);
            build.recordPath = u8str(record);
            build.profilePath = u8str(profile);
            build.keepCurrent = !otherEngine;
            {
                WorkerInstance worker(w.script, w.base, &engine.assetMounts(), /*workerId=*/0, std::string(),
                                      w.moduleScript);
                worker.setAotBuild(&build);
                worker.start();
                worker.waitForExit();
            }
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const bronze::eval::ModuleBuildResult& res = build.result;
            if (!build.scriptRead) {
                status = "failed: the worker script could not be read";
                allOk = false;
            } else if (build.current) {
                keep.insert(u8str(u8path(build.currentModule).filename()));
                status = "current";
                ++current;
            } else if (res.success) {
                keep.insert(u8str(module.filename()));
                char buf[64];
                std::snprintf(buf, sizeof(buf), "built %.0f ms", ms);
                status = std::string(buf) + " on its profile, " + std::to_string(res.coldFunctions) + " of " +
                         std::to_string(res.functions) + " functions cold";
                ++built;
            } else if (res.profileStale) {
                fs::remove(profile, ec);
                fs::remove(record, ec);
                status = "stale profile dropped, waiting for its profile";
                ++waiting;
            } else {
                status = "failed: " + oneLine(res.errorMessage);
                allOk = false;
                fs::remove(module, ec);
                fs::remove(record, ec);
                LOG_ERROR("aot: worker %s did not compile ahead of time, it will compile when it starts:\n%s",
                          w.script.c_str(), res.errorMessage.c_str());
            }
        }
        summary += "worker" + std::to_string(workerUnits++) + "=" + id + " " + status + " " + oneLine(w.script) +
                   "\n";
        LOG_INFO("aot: worker %s: %s", w.script.c_str(), status.c_str());
    }
    summary += "workers=" + std::to_string(workerUnits) + "\n";

    // Modules no record names: earlier builds'. One a running executable has
    // mapped refuses (Windows) or lives on unlinked (POSIX); either is fine.
    for (auto it = fs::directory_iterator(u8path(dir), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string n = u8str(it->path().filename());
        const bool isModule = n.size() > std::char_traits<char>::length(kModuleExt) &&
                              n.compare(n.size() - std::char_traits<char>::length(kModuleExt),
                                        std::string::npos, kModuleExt) == 0;
        if (isModule && !keep.count(n)) {
            std::error_code rec;
            fs::remove(it->path(), rec);
        }
    }
    summary += "built=" + std::to_string(built) + "\ncurrent=" + std::to_string(current) +
               "\nwaiting=" + std::to_string(waiting) + "\n";
    {
        const fs::path tmp = u8path(dir) / (std::string(kSummaryFile) + ".tmp");
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << summary;
        out.close();
        fs::rename(tmp, u8path(dir) / kSummaryFile, ec);
    }
    return allOk;
}

namespace {

// `bro --build-exe` for the running app's executable, in the background and
// once a process: the next launch has current modules. While a worker is
// harvesting, the request waits for it: the page's harvest is written at
// pagehide and the workers' as they end, after it, and one build is to read
// them all (settleWorkerAot makes the request when the last one ends).
std::mutex s_rebuildMutex;
int s_workersHarvesting = 0;
// Per harvest file, the instances still running that write it.
std::unordered_map<std::string, int> s_unitInstances;
bool s_rebuildWanted = false;
bool s_rebuildRequested = false;

std::vector<std::string> s_rebuildCommand;

void spawnAotRebuild(const std::string& appDir, const std::string& aotDir) {
    std::vector<std::string> argv;
    {
        std::lock_guard<std::mutex> lock(s_rebuildMutex);
        argv = s_rebuildCommand;
    }
    if (argv.empty()) {
        const std::string& exe = engine::currentApp().executable;
        if (exe.empty()) return;
        argv = {engine::stockBroPath(), "--build-exe", appDir, "--out", exe, "--quiet"};
    }
    std::string err;
    if (!util::spawnDetached(argv, {}, err)) {
        LOG_WARN("aot: cannot start the rebuild of %s: %s", aotDir.c_str(), err.c_str());
    }
}

void requestAotRebuild(engine::Engine& engine) {
    {
        std::lock_guard<std::mutex> lock(s_rebuildMutex);
        s_rebuildWanted = true;
        if (s_workersHarvesting > 0 || s_rebuildRequested) return;
        s_rebuildRequested = true;
    }
    spawnAotRebuild(engine.appDir(), engine.aotDir());
}

// Whether the last build of `dir` failed unit `id` (aot.txt): it is not
// asked for again until something changes.
bool unitBuildFailed(const std::string& dir, const std::string& id) {
    std::ifstream in(u8path(dir) / kSummaryFile, std::ios::binary);
    std::string line;
    while (in && std::getline(in, line)) {
        if (line.find("=" + id + " failed: ") != std::string::npos) return true;
    }
    return false;
}

// The units this process compiled to be harvested, and the modules built on
// a profile it runs (page thread only).
struct HarvestedRun {
    std::string name;
    fs::path profile;
};
std::vector<HarvestedRun> s_harvested;
struct ProfiledRun {
    std::string name;
    fs::path record;
    fs::path profile;
    const uint64_t* coldEntries = nullptr;
    uint32_t coldEntryCount = 0;
    double startMs = 0.0;
};
std::vector<ProfiledRun> s_profiled;

}  // namespace

void setAotRebuildCommand(std::vector<std::string> argv) {
    std::lock_guard<std::mutex> lock(s_rebuildMutex);
    s_rebuildCommand = std::move(argv);
}

std::optional<bronze::eval::LoadedModule> loadAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      bronze::eval::EvalOptions& opts, int ordinal, bool pageUnit) {
    // Scripts that are not the page's (a control eval, a driver) have no
    // module, and are not harvested either.
    if (engine.aotDir().empty() || isJitDisabled() || !pageUnit) return std::nullopt;
    const std::string& dir = engine.aotDir();
    const std::string id = aotUnitId(opts.filename, opts.publishEntry, ordinal);
    const fs::path record = recordPath(dir, id);
    const fs::path profile = profilePath(dir, id);
    std::error_code ec;
    const bool hasRecord = fs::is_regular_file(record, ec);
    const bool hasProfile = fs::is_regular_file(profile, ec);

    if (hasRecord && hasProfile) {
        // A module is built on its profile, and its record names the
        // profile's bytes among its inputs.
        bronze::eval::EvalOptions fed = opts;
        fed.feedbackInPath = u8str(profile);
        bronze::eval::LoadedModule loaded = bronze::eval::loadModule(execCode, fed, u8str(record));
        if (loaded.ok) {
            LOG_INFO("aot: %s runs its ahead-of-time module %s (built on its profile)", opts.filename.c_str(),
                     loaded.modulePath.c_str());
            if (loaded.coldEntries) {
                s_profiled.push_back({opts.filename, record, profile, loaded.coldEntries, loaded.coldEntryCount,
                                      util::currentTimeMs()});
            }
            return loaded;
        }
        if (engine.aotRequired()) {
            // The ahead-of-page load and the unit's own load both ask; say it once.
            static std::set<std::string> reported;  // page thread only
            if (reported.insert(opts.filename).second)
                LOG_ERROR("aot: --aot %s: the module for %s is out of date (%s); rebuild it with --aot-build",
                          dir.c_str(), opts.filename.c_str(), loaded.why.c_str());
            engine.setTestFailure(true);
        } else {
            LOG_WARN("aot: the ahead-of-time module for %s is out of date (%s); compiling it", opts.filename.c_str(),
                     loaded.why.c_str());
        }
        // The profile is a run of the program the record names, not of this
        // one: this run is harvested in its place.
        fs::remove(profile, ec);
        fs::remove(record, ec);
    } else if (hasProfile) {
        // Harvested, and the module not built yet: the build runs in the
        // background (or failed, which aot.txt says), and this run compiles
        // as a folder launch does.
        LOG_INFO("aot: %s has its profile and no module yet; compiling it", opts.filename.c_str());
        if (!unitBuildFailed(dir, id)) requestAotRebuild(engine);
        return std::nullopt;
    }

    // No module yet: the unit's first run (or its first since its text
    // changed) compiles in process to be harvested, and the module is built
    // on what it did (settleAotHarvests).
    // The page's first unit is asked ahead of the page and again when it runs.
    opts.harvestOutPath = u8str(profile);
    const auto same = [&](const HarvestedRun& h) { return h.profile == profile; };
    if (std::none_of(s_harvested.begin(), s_harvested.end(), same)) {
        s_harvested.push_back({opts.filename, profile});
        LOG_INFO("aot: no ahead-of-time module for %s yet: compiled in process, to be harvested for its build",
                 opts.filename.c_str());
    }
    return std::nullopt;
}

void settleAotHarvests(engine::Engine& engine) {
    std::error_code ec;
    bool any = false;
    for (const HarvestedRun& h : s_harvested) {
        if (!fs::is_regular_file(h.profile, ec)) continue;
        LOG_INFO("aot: %s: its run is harvested, and its module is built on it", h.name.c_str());
        any = true;
    }
    s_harvested.clear();
    // A module whose run entered often what its profile saw seldom or never:
    // the profile was too thin a run, and the next launch harvests the unit
    // again (no record: loadAotUnit).
    for (const ProfiledRun& p : s_profiled) {
        uint64_t most = 0;
        double seconds = 0.0;
        if (!profileTooThin(p.coldEntries, p.coldEntryCount, p.startMs, most, seconds)) continue;
        fs::remove(p.record, ec);
        fs::remove(p.profile, ec);
        LOG_INFO("aot: %s entered a function its profile saw cold %llu times in %.0f s: its next launch is "
                 "harvested again", p.name.c_str(), static_cast<unsigned long long>(most), seconds);
    }
    s_profiled.clear();
    if (any) requestAotRebuild(engine);
}

namespace {

struct AheadAot {
    std::string code;  // exactly what the unit would be compiled from
    std::string filename;
    bool publishEntry = false;
    bronze::eval::LoadedModule module;
};
std::optional<AheadAot> s_ahead;  // page thread only
// How many units of each name and kind this page has run (aotUnitId's
// ordinal); page thread only.
std::map<std::string, int> s_runOrdinals;

}  // namespace

bool loadAheadAotUnit(engine::Engine& engine, const std::string& execCode, bronze::eval::EvalOptions& opts) {
    if (engine.aotDir().empty()) return false;
    const double at = engine::sinceStartMs();
    auto loaded = loadAotUnit(engine, execCode, opts, 0, /*pageUnit=*/true);
    if (!loaded) return false;
    engine::notePageCompile(at, engine::sinceStartMs(), 0.0, "aot");
    s_ahead = AheadAot{execCode, opts.filename, opts.publishEntry, std::move(*loaded)};
    return true;
}

void beginAotUnitRuns() { s_runOrdinals.clear(); }

std::optional<bronze::eval::LoadedModule> takeAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      bronze::eval::EvalOptions& opts, bool pageUnit) {
    if (engine.aotDir().empty()) return std::nullopt;
    const int ordinal = s_runOrdinals[aotUnitId(opts.filename, opts.publishEntry)]++;
    // The module kept ahead of the page is its first unit's (ordinal 0).
    if (s_ahead && ordinal == 0 && s_ahead->filename == opts.filename &&
        s_ahead->publishEntry == opts.publishEntry && s_ahead->code == execCode) {
        bronze::eval::LoadedModule taken = std::move(s_ahead->module);
        s_ahead.reset();
        return taken;
    }
    return loadAotUnit(engine, execCode, opts, ordinal, pageUnit);
}

void discardAheadAotUnit() { s_ahead.reset(); }

std::optional<bronze::eval::LoadedModule> loadWorkerAotUnit(engine::Engine& engine, const std::string& code,
                                                            bronze::eval::EvalOptions& opts, bool moduleScript,
                                                            const std::string& basePath, WorkerAotRun& run) {
    if (engine.aotDir().empty() || isJitDisabled()) return std::nullopt;
    const std::string& dir = engine.aotDir();
    const std::string id = workerUnitId(opts.filename, moduleScript);
    const fs::path record = recordPath(dir, id);
    const fs::path profile = profilePath(dir, id);
    std::error_code ec;
    run.name = opts.filename;
    run.record = u8str(record);
    run.profile = u8str(profile);

    if (fs::is_regular_file(profile, ec)) {
        if (fs::is_regular_file(record, ec)) {
            bronze::eval::EvalOptions fed = opts;
            fed.feedbackInPath = u8str(profile);
            bronze::eval::LoadedModule loaded = bronze::eval::loadModule(code, fed, u8str(record));
            if (loaded.ok) {
                LOG_INFO("aot: worker %s runs its ahead-of-time module %s (built on its profile)",
                         opts.filename.c_str(), loaded.modulePath.c_str());
                run.coldEntries = loaded.coldEntries;
                run.coldEntryCount = loaded.coldEntryCount;
                run.startMs = util::currentTimeMs();
                return loaded;
            }
            LOG_WARN("aot: the ahead-of-time module for worker %s is out of date (%s); compiling it",
                     opts.filename.c_str(), loaded.why.c_str());
            fs::remove(profile, ec);
            fs::remove(record, ec);
        } else {
            // Harvested, and the module not built yet (or its build failed,
            // which aot.txt says): this run compiles as a folder launch does.
            LOG_INFO("aot: worker %s has its profile and no module yet; compiling it", opts.filename.c_str());
            if (!unitBuildFailed(dir, id)) requestAotRebuild(engine);
            return std::nullopt;
        }
    }

    // No module yet: this run compiles to be harvested. The harvest of one
    // script is one, whatever thread runs an instance of it (bronze keeps a
    // path per module, and a write is the union of the instances folded so
    // far), so every instance of a pool arms the same file; the last to end
    // renames it onto the profile.
    {
        std::ofstream side(workerSidecarPath(dir, id), std::ios::binary | std::ios::trunc);
        side << "script=" << opts.filename << "\nbase=" << basePath << "\nmodule=" << (moduleScript ? 1 : 0)
             << "\n";
    }
    run.harvestOut = u8str(profile) + ".tmp";
    run.appDir = engine.appDir();
    run.aotDir = dir;
    run.harvesting = true;
    opts.harvestOutPath = run.harvestOut;
    {
        std::lock_guard<std::mutex> lock(s_rebuildMutex);
        ++s_workersHarvesting;
        ++s_unitInstances[run.harvestOut];
    }
    LOG_INFO("aot: no ahead-of-time module for worker %s yet: compiled in process, to be harvested for its build",
             opts.filename.c_str());
    return std::nullopt;
}

void settleWorkerAot(WorkerAotRun& run) {
    std::error_code ec;
    if (run.coldEntries) {
        uint64_t most = 0;
        double seconds = 0.0;
        if (profileTooThin(run.coldEntries, run.coldEntryCount, run.startMs, most, seconds)) {
            fs::remove(u8path(run.record), ec);
            fs::remove(u8path(run.profile), ec);
            LOG_INFO("aot: worker %s entered a function its profile saw cold %llu times in %.0f s: it is harvested "
                     "again when it next starts",
                     run.name.c_str(), static_cast<unsigned long long>(most), seconds);
        }
        run.coldEntries = nullptr;
    }
    if (!run.harvesting) return;
    run.harvesting = false;
    bool last = false;
    {
        std::lock_guard<std::mutex> lock(s_rebuildMutex);
        auto it = s_unitInstances.find(run.harvestOut);
        last = it == s_unitInstances.end() || --it->second <= 0;
        if (last && it != s_unitInstances.end()) s_unitInstances.erase(it);
    }
    bool written = false;
    if (last && fs::is_regular_file(u8path(run.harvestOut), ec)) {
        fs::rename(u8path(run.harvestOut), u8path(run.profile), ec);
        written = !ec;
        if (ec) fs::remove(u8path(run.harvestOut), ec);
    }
    if (written) LOG_INFO("aot: worker %s: its run is harvested, and its module is built on it", run.name.c_str());
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lock(s_rebuildMutex);
        --s_workersHarvesting;
        if (written) s_rebuildWanted = true;
        if (s_workersHarvesting == 0 && s_rebuildWanted && !s_rebuildRequested) {
            s_rebuildRequested = true;
            spawn = true;
        }
    }
    if (spawn) spawnAotRebuild(run.appDir, run.aotDir);
}

}  // namespace bro::bronze_host
