#include "engine/app_loader.h"
#include "util/asset_mounts.h"
#include "util/log.h"
#include "util/remote_asset.h"
#include "util/string_utils.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>

namespace bro::engine {

namespace {

/// Does this `<script type>` name JavaScript? The HTML spec's "JavaScript MIME
/// type essence match", minus the parameters no page in practice writes. An
/// empty type means JavaScript — that is the common case, an ordinary
/// `<script>` with no type at all.
bool isExecutableScriptType(const std::string& type) {
    if (type.empty()) return true;
    static const char* kJavaScriptTypes[] = {
        "text/javascript",       "application/javascript",
        "text/ecmascript",       "application/ecmascript",
        "text/javascript1.0",    "text/javascript1.1",
        "text/javascript1.2",    "text/javascript1.3",
        "text/javascript1.4",    "text/javascript1.5",
        "text/jscript",          "text/livescript",
        "text/x-ecmascript",     "text/x-javascript",
        "application/x-ecmascript", "application/x-javascript",
    };
    for (const char* t : kJavaScriptTypes) {
        if (type == t) return true;
    }
    return false;
}

} // namespace

std::string AppLoader::loadFile(const std::string& path) {
    // An absolute URL is a resource on the network, not a file. Everything the
    // manifest carries — stylesheets, external scripts — funnels through here,
    // so this one branch is what makes `<script src="https://cdn/…">` mean what
    // the web says it means. Cached on disk; see util/remote_asset.h.
    if (util::isHttpUrl(path)) {
        std::string body = util::fetchRemoteCached(path);
        if (body.empty())
            LOG_ERROR("AppLoader::loadFile: cannot fetch '%s'", path.c_str());
        return body;
    }

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

    // An absolute URL resolves to itself. It must also skip the lexical
    // normalization below, which would collapse the "//" after the scheme and
    // turn separators around on Windows — leaving a string that is neither a
    // URL nor a path.
    if (util::hasUrlScheme(path)) return path;

    std::string result;
    bool driveAbsolute = path.size() >= 2 && path[1] == ':';
    if (driveAbsolute) {
        // Drive-letter absolute (Windows) — pass through.
        result = path;
    } else if (path[0] == '/' || path[0] == '\\') {
        // `/`-prefixed: try engine mounts first; otherwise treat as filesystem
        // absolute. Mounts take precedence so `/lib/foo.js` resolves through
        // the mount table even when `/lib` exists on disk.
        std::string m = mounts ? mounts->resolve(path) : std::string();
        result = m.empty() ? path : m;
    } else {
        // Bare relative — append to base.
        result = base;
        if (!result.empty() && result.back() != '/' && result.back() != '\\') {
            result += '/';
        }
        result += path;
    }

    // Canonicalize lexically, matching js::module_normalize (runtime.cpp)'s
    // treatment of the same "/"-prefixed specifiers. Without this, a
    // `<script type="module" src="/app/app.js">` tag and an `import ... from
    // "/app/app.js"` statement resolve to different-looking strings (this
    // function leaves mount-resolved paths with forward slashes, while
    // lexically_normal() rewrites them to the platform separator on
    // Windows), so QuickJS's specifier-keyed module cache treats the same
    // file as two distinct modules and re-executes it — silently doubling
    // any module-level singleton state.
    if (result.find('/') != std::string::npos || result.find('\\') != std::string::npos) {
        std::error_code ec;
        std::string normalized = std::filesystem::path(result).lexically_normal().string();
        if (!normalized.empty()) result = normalized;
    }
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

    // Extract all <script> tags in document order (both src="..." and inline).
    //
    // A <script>'s `type` decides whether it is code at all. Only a JavaScript
    // MIME type (or none) is executable; `module` is executable as an ES
    // module; `importmap` is *data* that steers module resolution; and
    // everything else — `application/json`, `text/x-template`,
    // `speculationrules` — is a data block the HTML spec says a browser must
    // not run. Running one is not a harmless no-op: an import map is JSON, and
    // JSON evaluated as JavaScript is a SyntaxError on the first `:`, which is
    // exactly what the page's real content looks like when it fails.
    {
        std::regex scriptRe(R"(<script([^>]*)>([\s\S]*?)</script>)",
                            std::regex_constants::icase);
        std::regex srcRe(R"(src\s*=\s*["']([^"']+)["'])",
                         std::regex_constants::icase);
        std::regex typeRe(R"(type\s*=\s*["']([^"']*)["'])",
                          std::regex_constants::icase);
        auto begin = std::sregex_iterator(html.begin(), html.end(), scriptRe);
        auto end = std::sregex_iterator();
        bool haveImportMap = false;
        for (auto it = begin; it != end; ++it) {
            std::string attrs = (*it)[1].str();
            std::string body = (*it)[2].str();

            std::string type;
            std::smatch typeMatch;
            if (std::regex_search(attrs, typeMatch, typeRe)) {
                type = util::trim(util::toLower(typeMatch[1].str()));
            }

            if (type == "importmap") {
                // One map per document (HTML ignores any later one), and it
                // only means anything inline — a src'd import map is invalid.
                if (haveImportMap) {
                    LOG_WARN("AppLoader: ignoring a second <script type=\"importmap\"> in "
                             "'%s' — a document has exactly one import map, and it is the "
                             "first.", manifest.htmlPath.c_str());
                    continue;
                }
                haveImportMap = true;
                if (!manifest.importMap.parse(body, appDir)) {
                    LOG_ERROR("AppLoader: <script type=\"importmap\"> in '%s' is not valid "
                              "JSON — no bare import specifier will resolve.",
                              manifest.htmlPath.c_str());
                } else {
                    LOG_INFO("AppLoader: import map with %zu entr%s",
                             manifest.importMap.size(),
                             manifest.importMap.size() == 1 ? "y" : "ies");
                }
                continue;
            }

            const bool isModule = (type == "module");
            if (!isModule && !isExecutableScriptType(type)) {
                // A data block. Skipped deliberately, and said out loud, so a
                // page whose <script type="text/x-template"> never ran does not
                // have to be discovered by its absence.
                LOG_INFO("AppLoader: skipping <script type=\"%s\"> (not executable)",
                         type.c_str());
                continue;
            }

            std::smatch srcMatch;
            if (std::regex_search(attrs, srcMatch, srcRe)) {
                // External script
                std::string resolved = resolvePath(appDir, srcMatch[1].str(), mounts);
                manifest.scripts.push_back({resolved, {}, isModule});
            } else if (!body.empty()) {
                // Inline script
                manifest.scripts.push_back({{}, body, isModule});
            }
        }
    }

    LOG_INFO("AppLoader: loaded manifest from '%s' (%zu styles, %zu scripts)",
             appDir.c_str(), manifest.stylePaths.size(), manifest.scripts.size());

    return manifest;
}

} // namespace bro::engine
