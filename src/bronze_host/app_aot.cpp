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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

std::string aotUnitId(const std::string& filename, bool publishEntry) {
    util::Sha256 h;
    const std::string material = std::string(publishEntry ? "module\n" : "script\n") + filename;
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
    std::set<std::string> keep;  // module files a record names
    std::string summary = "engine=" + engine::engineVersion() + "\n";
    summary += "units=" + std::to_string(units.size()) + "\n";
    bool allOk = true;
    int built = 0, current = 0;
    for (size_t i = 0; i < units.size(); ++i) {
        const auto& unit = units[i];
        bronze::eval::EvalOptions opts = scriptEvalOptions(engine, unit.name, unit.moduleFile);
        opts.externalModules = registry;
        opts.externalModulesCaptured = true;
        const std::string execCode = scriptExecCode(unit.code, unit.name);
        const std::string id = aotUnitId(opts.filename, opts.publishEntry);
        const std::string record = u8str(recordPath(dir, id));
        std::string status;

        const auto start = std::chrono::steady_clock::now();
        bronze::eval::LoadedModule existing = bronze::eval::loadModule(execCode, opts, record);
        if (existing.ok) {
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
                status = buf;
                ++built;
                for (const std::string& f : res.filesRead) {
                    if (std::find(registry.begin(), registry.end(), f) == registry.end()) registry.push_back(f);
                }
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
        if (existing.ok && unit.moduleFile) {
            // A current module's graph is what it was: its entry and imports
            // are published the same way, which only a build reports. The
            // entry is enough for the common shape (one module script).
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
    summary += "built=" + std::to_string(built) + "\ncurrent=" + std::to_string(current) + "\n";
    {
        const fs::path tmp = u8path(dir) / (std::string(kSummaryFile) + ".tmp");
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << summary;
        out.close();
        fs::rename(tmp, u8path(dir) / kSummaryFile, ec);
    }
    return allOk;
}

std::optional<bronze::eval::LoadedModule> loadAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      const bronze::eval::EvalOptions& opts) {
    if (engine.aotDir().empty() || isJitDisabled()) return std::nullopt;
    const std::string id = aotUnitId(opts.filename, opts.publishEntry);
    const fs::path record = recordPath(engine.aotDir(), id);
    std::error_code ec;
    // Scripts that are not the page's (a control eval, a driver) have no
    // record, and that is not staleness.
    if (!fs::is_regular_file(record, ec)) {
        LOG_INFO("aot: no ahead-of-time module for %s in %s", opts.filename.c_str(), engine.aotDir().c_str());
        return std::nullopt;
    }

    bronze::eval::LoadedModule loaded = bronze::eval::loadModule(execCode, opts, u8str(record));
    if (loaded.ok) {
        LOG_INFO("aot: %s runs its ahead-of-time module %s", opts.filename.c_str(), loaded.modulePath.c_str());
        return loaded;
    }
    LOG_WARN("aot: the ahead-of-time module for %s is out of date (%s); compiling it", opts.filename.c_str(),
             loaded.why.c_str());
    static bool rebuildRequested = false;
    const std::string& exe = engine::currentApp().executable;
    if (!rebuildRequested && !exe.empty()) {
        rebuildRequested = true;
        std::string err;
        if (!util::spawnDetached({engine::stockBroPath(), "--build-exe", engine.appDir(), "--out", exe, "--quiet"},
                                 {}, err)) {
            LOG_WARN("aot: cannot start the rebuild of %s: %s", engine.aotDir().c_str(), err.c_str());
        }
    }
    return std::nullopt;
}

namespace {

struct AheadAot {
    std::string code;  // exactly what the unit would be compiled from
    std::string filename;
    bool publishEntry = false;
    bronze::eval::LoadedModule module;
};
std::optional<AheadAot> s_ahead;  // page thread only

}  // namespace

bool loadAheadAotUnit(engine::Engine& engine, const std::string& execCode, const bronze::eval::EvalOptions& opts) {
    if (engine.aotDir().empty()) return false;
    const double at = engine::sinceStartMs();
    auto loaded = loadAotUnit(engine, execCode, opts);
    if (!loaded) return false;
    engine::notePageCompile(at, engine::sinceStartMs(), 0.0, "aot");
    s_ahead = AheadAot{execCode, opts.filename, opts.publishEntry, std::move(*loaded)};
    return true;
}

std::optional<bronze::eval::LoadedModule> takeAotUnit(engine::Engine& engine, const std::string& execCode,
                                                      const bronze::eval::EvalOptions& opts) {
    if (s_ahead && s_ahead->filename == opts.filename && s_ahead->publishEntry == opts.publishEntry &&
        s_ahead->code == execCode) {
        bronze::eval::LoadedModule taken = std::move(s_ahead->module);
        s_ahead.reset();
        return taken;
    }
    return loadAotUnit(engine, execCode, opts);
}

void discardAheadAotUnit() { s_ahead.reset(); }

}  // namespace bro::bronze_host
