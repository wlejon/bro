// An installed app's page scripts compiled ahead of time: the build inside
// `bro-headless --aot-build`, and the lookup each unit makes at launch.
// app_aot.h holds the reasoning.

#include "bronze_host/app_aot.h"

#include "bronze_host/bronze_host.h"
#include "bronze_host/eval_jit.h"
#include "bronze_host/host_gc.h"
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
#include <set>
#include <system_error>

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
        bronze::eval::EvalOptions opts = scriptEvalOptions(engine, unit.name, unit.moduleFile);
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
// once a process: the next launch has current modules.
void requestAotRebuild(engine::Engine& engine) {
    static bool requested = false;  // page thread only
    const std::string& exe = engine::currentApp().executable;
    if (requested || exe.empty()) return;
    requested = true;
    std::string err;
    if (!util::spawnDetached({engine::stockBroPath(), "--build-exe", engine.appDir(), "--out", exe, "--quiet"}, {},
                             err)) {
        LOG_WARN("aot: cannot start the rebuild of %s: %s", engine.aotDir().c_str(), err.c_str());
    }
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
    const double now = util::currentTimeMs();
    for (const ProfiledRun& p : s_profiled) {
        uint64_t most = 0;
        for (uint32_t i = 0; i < p.coldEntryCount; ++i) most = std::max(most, p.coldEntries[i]);
        const double seconds = std::max(0.0, now - p.startMs) / 1000.0;
        const uint64_t limit = std::max(kColdEntriesMin, static_cast<uint64_t>(seconds * kColdEntriesPerSecond));
        if (most < limit) continue;
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

}  // namespace bro::bronze_host
