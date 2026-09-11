#include "util/asset_path.h"
#include "util/asset_mounts.h"
#include <filesystem>

namespace bro::util {

namespace {
std::string s_basePath;
const AssetMounts* s_mounts = nullptr;
}

void setAssetPathContext(const std::string& basePath, const AssetMounts* mounts) {
    s_basePath = basePath;
    s_mounts = mounts;
}

std::string resolveAssetPath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;           // C:\... drive
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) {
        if (s_mounts) {
            std::string m = s_mounts->resolve(src);
            if (!m.empty()) return m;
        }
        // An unmounted absolute path is already a filesystem path.
        return src;
    }
    if (s_basePath.empty()) return src;
    std::string path = s_basePath;
    if (path.back() != '/' && path.back() != '\\') path += '/';
    return path + src;
}

std::string resolveAssetWritePath(const std::string& src) {
    namespace fs = std::filesystem;
    fs::path p(src);
    if (p.is_absolute() || !p.has_parent_path()) return resolveAssetPath(src);
    std::string dir = resolveAssetPath(p.parent_path().generic_string());
    return (fs::path(dir) / p.filename()).generic_string();
}

} // namespace bro::util
