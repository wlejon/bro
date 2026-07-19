#include "js/asset_path.h"

#include "util/asset_mounts.h"

#include <filesystem>

namespace bro::js {

namespace {
std::string s_basePath;
const util::AssetMounts* s_mounts = nullptr;
}

void setAssetPathContext(const std::string& basePath, const util::AssetMounts* mounts) {
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
    // No parent to resolve (a bare filename), or already absolute: the read
    // rules are exactly right.
    if (p.is_absolute() || !p.has_parent_path()) return resolveAssetPath(src);
    // Resolve the DIRECTORY and rejoin the filename. Resolving the whole path
    // would ask a mount to resolve a file that does not exist yet, which is
    // the one thing the read path cannot answer.
    std::string dir = resolveAssetPath(p.parent_path().generic_string());
    return (fs::path(dir) / p.filename()).generic_string();
}

} // namespace bro::js
