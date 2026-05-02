#include "engine/app_loader.h"
#include "util/asset_mounts.h"
#include "util/log.h"
#include <fstream>
#include <sstream>
#include <regex>

namespace bro::engine {

std::string AppLoader::loadFile(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        LOG_ERROR("AppLoader::loadFile: cannot open '%s'", path.c_str());
        return {};
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string AppLoader::resolvePath(const std::string& base,
                                   const std::string& path,
                                   const util::AssetMounts* mounts)
{
    if (path.empty()) return base;

    // Drive-letter absolute (Windows) — pass through.
    if (path.size() >= 2 && path[1] == ':') return path;

    // `/`-prefixed: try engine mounts first; otherwise treat as filesystem
    // absolute. Mounts take precedence so `/lib/foo.js` resolves through
    // the mount table even when `/lib` exists on disk.
    if (path[0] == '/' || path[0] == '\\') {
        if (mounts) {
            std::string m = mounts->resolve(path);
            if (!m.empty()) return m;
        }
        return path;
    }

    // Bare relative — append to base.
    std::string result = base;
    if (!result.empty() && result.back() != '/' && result.back() != '\\') {
        result += '/';
    }
    result += path;
    return result;
}

AppManifest AppLoader::loadApp(const std::string& appDir, const util::AssetMounts* mounts) {
    AppManifest manifest;
    manifest.basePath = appDir;
    manifest.htmlPath = resolvePath(appDir, "index.html", mounts);

    std::string html = loadFile(manifest.htmlPath);
    if (html.empty()) {
        LOG_ERROR("AppLoader::loadApp: no index.html found in '%s'", appDir.c_str());
        return manifest;
    }

    // Extract <link rel="stylesheet" href="...">
    {
        std::regex linkRe(R"(<link[^>]+rel\s*=\s*["']stylesheet["'][^>]+href\s*=\s*["']([^"']+)["'])",
                          std::regex_constants::icase);
        auto begin = std::sregex_iterator(html.begin(), html.end(), linkRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string href = (*it)[1].str();
            manifest.stylePaths.push_back(resolvePath(appDir, href, mounts));
        }
    }

    // Also match href before rel (common ordering)
    {
        std::regex linkRe2(R"(<link[^>]+href\s*=\s*["']([^"']+)["'][^>]+rel\s*=\s*["']stylesheet["'])",
                           std::regex_constants::icase);
        auto begin = std::sregex_iterator(html.begin(), html.end(), linkRe2);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string href = (*it)[1].str();
            std::string resolved = resolvePath(appDir, href, mounts);
            // Avoid duplicates
            bool found = false;
            for (auto& existing : manifest.stylePaths) {
                if (existing == resolved) { found = true; break; }
            }
            if (!found) {
                manifest.stylePaths.push_back(resolved);
            }
        }
    }

    // Extract all <script> tags in document order (both src="..." and inline)
    {
        std::regex scriptRe(R"(<script([^>]*)>([\s\S]*?)</script>)",
                            std::regex_constants::icase);
        std::regex srcRe(R"(src\s*=\s*["']([^"']+)["'])",
                         std::regex_constants::icase);
        auto begin = std::sregex_iterator(html.begin(), html.end(), scriptRe);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) {
            std::string attrs = (*it)[1].str();
            std::string body = (*it)[2].str();
            std::smatch srcMatch;
            if (std::regex_search(attrs, srcMatch, srcRe)) {
                // External script
                std::string resolved = resolvePath(appDir, srcMatch[1].str(), mounts);
                manifest.scripts.push_back({resolved, {}});
            } else if (!body.empty()) {
                // Inline script
                manifest.scripts.push_back({{}, body});
            }
        }
    }

    LOG_INFO("AppLoader: loaded manifest from '%s' (%zu styles, %zu scripts)",
             appDir.c_str(), manifest.stylePaths.size(), manifest.scripts.size());

    return manifest;
}

} // namespace bro::engine
