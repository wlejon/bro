#include "util/asset_path.h"
#include "util/asset_mounts.h"
#include <filesystem>

namespace bro::util {

namespace {
std::string s_basePath;
const AssetMounts* s_mounts = nullptr;
std::string s_appUrlRoot;

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}
}  // namespace

void setAppUrlRoot(const std::string& appDir) { s_appUrlRoot = appDir; }

std::string pathForAppUrl(const std::string& url) {
    namespace fs = std::filesystem;
    static const std::string kRoot = "bro://app/";
    if (s_appUrlRoot.empty() || url.compare(0, kRoot.size(), kRoot) != 0) return {};
    std::string rel = url.substr(kRoot.size());
    if (const size_t cut = rel.find_first_of("?#"); cut != std::string::npos) rel.erase(cut);
    rel = percentDecode(rel);
    // A drive path reflected under the root ("bro://app/C:/x") is looked for
    // in the folder as "C/x", as "/home/x" is as "home/x": not there, it is
    // the path again (appUrlPath).
    if (isDrivePath(rel)) rel.erase(1, 1);
    std::error_code ec;
    const fs::path base = fs::absolute(s_appUrlRoot, ec).lexically_normal();
    const fs::path p = (base / rel).lexically_normal();
    if (p == base) return p.string();
    const fs::path inside = p.lexically_relative(base);
    if (inside.empty() || *inside.begin() == "..") return {};
    return p.string();
}

std::string appUrlPath(const std::string& url) {
    static const std::string kRoot = "bro://app/";
    if (url.compare(0, kRoot.size(), kRoot) != 0) return {};
    std::string path = url.substr(kRoot.size() - 1);
    if (const size_t cut = path.find_first_of("?#"); cut != std::string::npos) path.erase(cut);
    path = percentDecode(path);
    // "bro://app/C:/x" spells the drive path "C:/x", not "/C:/x".
    if (isDrivePath(path.substr(1))) path.erase(0, 1);
    return path;
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
