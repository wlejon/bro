#pragma once

#include <string>
#include <vector>

namespace bro::util { class AssetMounts; }

namespace bro::engine {

struct ScriptEntry {
    std::string path;    // non-empty for <script src="...">
    std::string code;    // non-empty for inline <script>...</script>
    bool isInline() const { return path.empty(); }
};

struct AppManifest {
    std::string htmlPath;
    std::string basePath;
    std::vector<ScriptEntry> scripts;    // in document order (external + inline)
    std::vector<std::string> stylePaths;
};

class AppLoader {
public:
    /// Read the entire contents of a file into a string.
    static std::string loadFile(const std::string& path);

    /// Resolve a path against a base directory and asset mounts.
    /// Resolution order: absolute disk path > engine-supplied mount (`/lib`,
    /// `/system`, ...) > base + relative. `mounts` may be null.
    static std::string resolvePath(const std::string& base,
                                   const std::string& path,
                                   const util::AssetMounts* mounts = nullptr);

    /// Load an application from a directory.
    /// Looks for index.html, extracts stylesheet and script references.
    /// `mounts`, when supplied, is consulted for `/<prefix>/...` paths.
    static AppManifest loadApp(const std::string& appDir,
                               const util::AssetMounts* mounts = nullptr);
};

} // namespace bro::engine
