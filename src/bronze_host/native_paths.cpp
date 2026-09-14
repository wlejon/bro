// `__bro_native.paths` — behind bro.appDir, bro.userDataDir and
// bro.resolvePath (docs/paths-api.js documents them as bro.paths.*; the live
// names are the bare ones on `bro`, and js/bro_core.js mounts them there).

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "util/asset_path.h"
#include "util/user_dirs.h"

#include <filesystem>
#include <system_error>

namespace bro::bronze_host {

namespace {

const char* appDirGet() {
    auto* eng = hostEngine();
    std::string dir;
    if (eng && !eng->appDir().empty()) {
        dir = std::filesystem::path(eng->appDir()).make_preferred().string();
    }
    return natives::strResult(std::move(dir));
}

// Created on first read, as the documentation promises: an app reads it to
// write into it.
const char* userDataDirGet() {
    auto* eng = hostEngine();
    const std::string base = eng ? eng->appDir() : std::string();
    std::string dir = util::appUserDataDir(base);
    if (dir.empty()) return natives::strResult(std::string());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return natives::strResult(std::filesystem::path(dir).make_preferred().string());
}

const char* resolvePath(const char* src) {
    const std::string resolved = util::resolveAssetPath(src);
    return natives::strResult(std::filesystem::path(resolved).make_preferred().string());
}

}  // namespace

bool registerPathsNatives(std::string* error) {
    using namespace natives;
    return getter("__bro_native.paths.appDir", reinterpret_cast<void*>(&appDirGet), "str", error) &&
           getter("__bro_native.paths.userDataDir", reinterpret_cast<void*>(&userDataDirGet), "str", error) &&
           fn("__bro_native.paths.resolvePath", reinterpret_cast<void*>(&resolvePath), "str", {"str"}, error);
}

}  // namespace bro::bronze_host
