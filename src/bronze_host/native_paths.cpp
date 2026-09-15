// `__bro_native.paths` — behind bro.appDir, bro.userDataDir and
// bro.resolvePath (docs/paths-api.js documents them as bro.paths.*; the live
// names are the bare ones on `bro`, and js/bro_core.js mounts them there).

#include "bronze_host/host_internal.h"
#include "bronze_host/host_natives.h"
#include "engine/engine.h"
#include "natives/paths/native_paths_decl.h"
#include "util/asset_path.h"
#include "util/user_dirs.h"

#include <filesystem>
#include <system_error>

extern "C" {

const char* bro_paths_appDir_get(void) {
    auto* eng = bro::bronze_host::hostEngine();
    std::string dir;
    if (eng && !eng->appDir().empty()) {
        dir = std::filesystem::path(eng->appDir()).make_preferred().string();
    }
    return bro::bronze_host::natives::strResult(std::move(dir));
}

const char* bro_paths_userDataDir_get(void) {
    auto* eng = bro::bronze_host::hostEngine();
    const std::string base = eng ? eng->appDir() : std::string();
    std::string dir = bro::util::appUserDataDir(base);
    if (dir.empty()) return bro::bronze_host::natives::strResult(std::string());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return bro::bronze_host::natives::strResult(std::filesystem::path(dir).make_preferred().string());
}

const char* bro_paths_resolvePath(const char* path) {
    const std::string resolved = bro::util::resolveAssetPath(path ? path : "");
    return bro::bronze_host::natives::strResult(std::filesystem::path(resolved).make_preferred().string());
}

const char* bro_paths_resolveWritePath(const char* path) {
    const std::string resolved = bro::util::resolveAssetWritePath(path ? path : "");
    return bro::bronze_host::natives::strResult(std::filesystem::path(resolved).make_preferred().string());
}

}  // extern "C"

namespace bro::bronze_host {

bool registerNatives_paths(std::string* error);

bool registerPathsNatives(std::string* error) {
    return registerNatives_paths(error);
}

}  // namespace bro::bronze_host
