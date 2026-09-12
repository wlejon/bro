#include "bronze_host/bronze_host.h"
#include "bronze_host/eval.h"
#include "bronze_host/app_module.h"
#include "embed/embed.h"
#include "cli/driver.h"
#include "engine/engine.h"
#include "util/log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace bro::bronze_host {

namespace {

static std::mutex s_netSyncMutex;
static std::string s_cachedNetSyncDll;
static std::atomic<uint64_t> s_netSyncInstSeq{1};

#ifdef _WIN32
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModuleExt = ".so";
#endif

static std::filesystem::path findNetSyncJs() {
    std::error_code ec;
    if (const char* env = std::getenv("BRO_PROJECT_ROOT")) {
        auto p = std::filesystem::path(env) / "src/bronze_host/net_sync.js";
        if (std::filesystem::exists(p, ec)) return p;
    }
    const auto exeDir = getExecutableDirectory();
    for (const auto& base : {
        exeDir,
        exeDir.parent_path(),
        exeDir.parent_path().parent_path(),
        std::filesystem::current_path(),
        std::filesystem::current_path().parent_path(),
    }) {
        for (const auto& rel : {
            "src/bronze_host/net_sync.js",
            "net_sync.js",
            "bronze/net_sync.js",
        }) {
            auto p = base / rel;
            if (std::filesystem::exists(p, ec)) return p;
        }
    }
    return "src/bronze_host/net_sync.js";
}

} // namespace

void installNetSync(engine::Engine* eng) {
    (void)eng;
    ensureSharedRuntimeEnv();

    std::error_code ec;
    std::string cachedDll;
    {
        std::lock_guard<std::mutex> lock(s_netSyncMutex);
        if (!s_cachedNetSyncDll.empty() && std::filesystem::exists(s_cachedNetSyncDll, ec)) {
            cachedDll = s_cachedNetSyncDll;
        } else {
            std::filesystem::path jsPath = findNetSyncJs();
            if (!std::filesystem::exists(jsPath, ec)) {
                LOG_ERROR("[net_sync] net_sync.js not found at: %s", jsPath.string().c_str());
                return;
            }

            std::filesystem::path tempDir = getEvalTempDir();
            std::string outDll = (tempDir / ("net_sync_cached" + std::string(kModuleExt))).string();
            std::string globalsPath = getWebHostGlobalsPath();
            std::string err;

            int status = bronze::cli::runBuild(
                jsPath.string(), outDll, &err,
                /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
                /*hostGlobals=*/globalsPath, /*inferStats=*/false,
                /*statsOut=*/nullptr, /*moduleRoots=*/{}, /*entrySymbol=*/{},
                /*emitShared=*/true, /*retainFnSource=*/true);

            if (status == 0 && std::filesystem::exists(outDll, ec)) {
                s_cachedNetSyncDll = outDll;
                cachedDll = outDll;
            } else {
                LOG_ERROR("[net_sync] compile failed: %s", err.c_str());
                return;
            }
        }
    }

    if (!cachedDll.empty()) {
        std::filesystem::path tempDir = getEvalTempDir();
        uint64_t instId = s_netSyncInstSeq.fetch_add(1, std::memory_order_relaxed);
        auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string instName = "net_sync_inst_" + std::to_string(instId) + "_" + std::to_string(ts) + kModuleExt;
        std::filesystem::path instDll = tempDir / instName;
        std::filesystem::copy_file(cachedDll, instDll, std::filesystem::copy_options::overwrite_existing, ec);

        std::string loadErr;
        ModuleHandle mod = openModule(instDll.string(), loadErr);
        if (mod) {
            auto entry = reinterpret_cast<void (*)()>(moduleSymbol(mod, "bronze_main"));
            if (entry) {
                bronze::embed::runEntry(entry);
            } else {
                LOG_ERROR("[net_sync] bronze_main not found in %s", instDll.string().c_str());
            }
        } else {
            LOG_ERROR("[net_sync] failed to open module %s: %s", instDll.string().c_str(), loadErr.c_str());
        }
    }
}

} // namespace bro::bronze_host
