#include "util/asset_mounts.h"

namespace bro::util {

static std::string stripTrailingSlash(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

void AssetMounts::addMount(const std::string& prefix, const std::string& absPath) {
    if (prefix.empty() || prefix[0] != '/') return;
    mounts_[prefix] = stripTrailingSlash(absPath);
}

bool AssetMounts::isMountedPath(const std::string& path) const {
    if (path.empty() || path[0] != '/') return false;
    for (const auto& [prefix, _] : mounts_) {
        if (path.size() >= prefix.size() &&
            path.compare(0, prefix.size(), prefix) == 0 &&
            (path.size() == prefix.size() || path[prefix.size()] == '/'))
        {
            return true;
        }
    }
    return false;
}

std::string AssetMounts::resolve(const std::string& path) const {
    if (path.empty() || path[0] != '/') return {};

    // Find the longest matching prefix (so /libfoo doesn't match /lib).
    const std::string* bestPrefix = nullptr;
    const std::string* bestPath = nullptr;
    for (const auto& [prefix, target] : mounts_) {
        if (path.size() >= prefix.size() &&
            path.compare(0, prefix.size(), prefix) == 0 &&
            (path.size() == prefix.size() || path[prefix.size()] == '/'))
        {
            if (!bestPrefix || prefix.size() > bestPrefix->size()) {
                bestPrefix = &prefix;
                bestPath = &target;
            }
        }
    }
    if (!bestPrefix) return {};

    // path begins with *bestPrefix; strip it and append the remainder to target.
    std::string remainder = path.substr(bestPrefix->size());
    return *bestPath + remainder;
}

} // namespace bro::util
