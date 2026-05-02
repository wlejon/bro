#pragma once

#include <string>
#include <unordered_map>

namespace bro::util {

/// Engine-supplied virtual path prefixes — `/lib`, `/system`, etc. — that
/// resolve to absolute disk directories. Apps reference shared resources
/// through these prefixes regardless of where they live on disk.
///
/// Resolution order (caller's responsibility — addMount overwrites):
///   1. App-local override (appDir/<dir-name> if it exists) — highest priority
///   2. Project root mount (root/<dir-name> from project bro.json)
///   3. Unmounted — `resolve` returns empty; caller errors out
///
/// Mount prefixes always start with `/` and never end with `/`. The
/// trailing portion of an asset path is appended directly.
class AssetMounts {
public:
    /// Add or overwrite a mount. `prefix` must start with `/`. `absPath`
    /// must be an absolute filesystem path. No existence check here —
    /// caller is expected to verify (e.g. only mount if dir exists).
    void addMount(const std::string& prefix, const std::string& absPath);

    /// True if `path` begins with a known mount prefix followed by `/` or end.
    bool isMountedPath(const std::string& path) const;

    /// If `path` begins with a known mount prefix, return the rewritten
    /// absolute disk path. Otherwise return empty string.
    std::string resolve(const std::string& path) const;

    /// Read access for cross-runtime registration (e.g. brokit fs/fetch).
    const std::unordered_map<std::string, std::string>& mounts() const { return mounts_; }

private:
    // Map: "/lib" -> "C:/path/to/lib" (no trailing slash).
    std::unordered_map<std::string, std::string> mounts_;
};

} // namespace bro::util
