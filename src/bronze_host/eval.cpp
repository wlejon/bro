#include "bronze_host/eval.h"
#include "bronze_host/bronze_host.h"
#include "bronze_host/app_module.h"
#include "bronze_host/host_headless.h"

#include "cli/driver.h"
#include "embed/embed.h"
#include "engine/engine.h"
#include "engine/engine_init_cabi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include "util/log.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bro::bronze_host {
namespace {

static std::atomic<uint64_t> s_evalCounter{0};

#ifdef _WIN32
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModuleExt = ".so";
#endif

std::filesystem::path getEvalTempDir() {
    std::filesystem::path p = std::filesystem::temp_directory_path() / "bro_eval";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

void ensureSharedRuntimeEnv() {
    if (std::getenv("BRONZE_SHARED_RT_LIB")) return;
    std::error_code ec;
    for (const auto& cand : {
        std::filesystem::path("build/Release/bronze_runtime_shared.lib"),
        std::filesystem::path("build/shared/Release/bronze_runtime_shared.lib"),
        std::filesystem::path("D:/projects/bro/build/Release/bronze_runtime_shared.lib"),
        std::filesystem::path("D:/projects/bro/build/shared/Release/bronze_runtime_shared.lib"),
    }) {
        if (std::filesystem::exists(cand, ec)) {
            std::string abs = std::filesystem::absolute(cand, ec).string();
#ifdef _WIN32
            _putenv_s("BRONZE_SHARED_RT_LIB", abs.c_str());
#else
            setenv("BRONZE_SHARED_RT_LIB", abs.c_str(), 1);
#endif
            break;
        }
    }
}

} // namespace

std::string getWebHostGlobalsPath() {
    std::error_code ec;
    if (const char* env = std::getenv("BRO_PROJECT_ROOT")) {
        auto p = std::filesystem::path(env) / "src/bronze_host/web_host.globals";
        if (std::filesystem::exists(p, ec)) return std::filesystem::absolute(p, ec).string();
    }
    for (const auto& rel : {
        "src/bronze_host/web_host.globals",
        "web_host.globals",
        "bronze/web_host.globals",
        "../src/bronze_host/web_host.globals",
        "../../src/bronze_host/web_host.globals",
    }) {
        auto p = std::filesystem::path(rel);
        if (std::filesystem::exists(p, ec)) return std::filesystem::absolute(p, ec).string();
    }
    auto projPath = std::filesystem::path("D:/projects/bro/src/bronze_host/web_host.globals");
    if (std::filesystem::exists(projPath, ec)) return projPath.string();

    return "src/bronze_host/web_host.globals";
}

bool evalScript(engine::Engine& engine, const std::string& code,
                const std::string& filename) {
    (void)filename;
    ensureSharedRuntimeEnv();

    const auto tempDir = getEvalTempDir();
    const uint64_t id = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem = "eval_" + std::to_string(ts) + "_" + std::to_string(id);

    const auto tempJs = tempDir / (stem + ".js");
    const auto outDll = tempDir / (stem + kModuleExt);

    {
        std::ofstream ofs(tempJs, std::ios::binary);
        if (!ofs.is_open()) {
            LOG_ERROR("eval: failed to write temporary file: %s", tempJs.string().c_str());
            setTestFailure(true);
            engine.setTestFailure(true);
            return false;
        }
        ofs.write(code.data(), code.size());
    }

    const std::string globalsPath = getWebHostGlobalsPath();
    std::string err;
    int status = bronze::cli::runBuild(
        tempJs.string(), outDll.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobals=*/globalsPath, /*inferStats=*/false,
        /*statsOut=*/nullptr, /*moduleRoots=*/{}, /*entrySymbol=*/{},
        /*emitShared=*/true, /*retainFnSource=*/true,
        /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{},
        /*pinsAllowObserved=*/false);

    std::error_code ec;
    std::filesystem::remove(tempJs, ec);

    if (status != 0) {
        LOG_ERROR("eval compilation failed:\n%s", err.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    std::string loadErr;
    ModuleHandle handle = openModule(outDll.string(), loadErr);
    if (!handle) {
        LOG_ERROR("eval module load failed: %s", loadErr.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    bro::engine::bro_engine_register_cabi_bridges(&engine);
    bro_c_abi_sync_bridges_to_module(handle);

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, "bronze_main"));
    if (!entry) {
        LOG_ERROR("eval module exports no bronze_main: %s", outDll.string().c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    bronze::embed::runEntry(entry);
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

bool evalScriptFile(engine::Engine& engine, const std::string& filePath) {
    ensureSharedRuntimeEnv();

    std::error_code ec;
    if (!std::filesystem::exists(filePath, ec)) {
        LOG_ERROR("evalScriptFile: file does not exist: %s", filePath.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    const auto absSource = std::filesystem::absolute(filePath, ec);
    const auto tempDir = getEvalTempDir();
    const uint64_t id = s_evalCounter.fetch_add(1, std::memory_order_relaxed);
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string stem = "script_" + std::to_string(ts) + "_" + std::to_string(id);
    const auto outDll = tempDir / (stem + kModuleExt);

    const std::string globalsPath = getWebHostGlobalsPath();
    std::string err;
    int status = bronze::cli::runBuild(
        absSource.string(), outDll.string(), &err,
        /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobals=*/globalsPath, /*inferStats=*/false,
        /*statsOut=*/nullptr, /*moduleRoots=*/{}, /*entrySymbol=*/{},
        /*emitShared=*/true, /*retainFnSource=*/true,
        /*importMapPath=*/{}, /*assumeNoBigInt=*/false,
        /*pinsPath=*/{}, /*censusOutPath=*/{},
        /*pinsAllowObserved=*/false);

    if (status != 0) {
        LOG_ERROR("evalScriptFile compilation failed for %s:\n%s", filePath.c_str(), err.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    std::string loadErr;
    ModuleHandle handle = openModule(outDll.string(), loadErr);
    if (!handle) {
        LOG_ERROR("evalScriptFile module load failed: %s", loadErr.c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    bro::engine::bro_engine_register_cabi_bridges(&engine);
    bro_c_abi_sync_bridges_to_module(handle);

    auto entry = reinterpret_cast<void (*)()>(moduleSymbol(handle, "bronze_main"));
    if (!entry) {
        LOG_ERROR("evalScriptFile module exports no bronze_main: %s", outDll.string().c_str());
        setTestFailure(true);
        engine.setTestFailure(true);
        return false;
    }

    if (!isWebHostGlobalsInstalled()) {
        installWebHostGlobals(engine);
    }

    bronze::embed::runEntry(entry);
    std::fflush(stdout);

    if (hasTestFailure() || engine.hasTestFailure()) {
        return false;
    }
    return true;
}

} // namespace bro::bronze_host
