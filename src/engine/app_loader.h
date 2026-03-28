#pragma once

#include <string>
#include <vector>

namespace bro::engine {

struct AppManifest {
    std::string htmlPath;
    std::string basePath;
    std::vector<std::string> scriptPaths;
    std::vector<std::string> stylePaths;
};

class AppLoader {
public:
    /// Read the entire contents of a file into a string.
    static std::string loadFile(const std::string& path);

    /// Resolve a relative path against a base directory.
    static std::string resolveRelativePath(const std::string& base, const std::string& relative);

    /// Load an application from a directory.
    /// Looks for index.html, extracts stylesheet and script references.
    static AppManifest loadApp(const std::string& appDir);
};

} // namespace bro::engine
