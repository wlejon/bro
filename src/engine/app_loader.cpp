#include "engine/app_loader.h"
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

std::string AppLoader::resolveRelativePath(const std::string& base, const std::string& relative) {
    if (relative.empty()) return base;

    // If relative is already an absolute path, return as-is.
    if (relative.size() >= 2 && relative[1] == ':') return relative;   // Windows absolute
    if (relative[0] == '/') return relative;                            // Unix absolute

    std::string result = base;
    if (!result.empty() && result.back() != '/' && result.back() != '\\') {
        result += '/';
    }
    result += relative;
    return result;
}

AppManifest AppLoader::loadApp(const std::string& appDir) {
    AppManifest manifest;
    manifest.basePath = appDir;
    manifest.htmlPath = resolveRelativePath(appDir, "index.html");

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
            manifest.stylePaths.push_back(resolveRelativePath(appDir, href));
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
            std::string resolved = resolveRelativePath(appDir, href);
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
                std::string resolved = resolveRelativePath(appDir, srcMatch[1].str());
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
