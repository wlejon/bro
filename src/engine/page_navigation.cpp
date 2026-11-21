// A document's URL and its navigation (location.href = ..., assign, replace).
// The app's pages are its folder's files under `bro://app/`: the app document
// shows one of them (pageUrl_, the folder's index.html at first), and
// navigating it loads another in its place, as a reload of the app realm.
// An iframe's document navigates its frame alone.

#include "engine/engine.h"
#include "engine/app_loader.h"
#include "platform/system_info.h"
#include "util/asset_path.h"
#include "util/log.h"
#include "util/remote_asset.h"

#include <filesystem>

namespace bro::engine {

namespace fs = std::filesystem;

namespace {

constexpr const char* kAppUrlRoot = "bro://app/";

std::string percentEncodePath(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c <= 0x20 || c == '%' || c == '?' || c == '#' || c >= 0x7F) {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

// Whether `path` (lexically normal) lies in `root` (lexically normal).
bool isWithin(const fs::path& path, const fs::path& root) {
    const fs::path rel = path.lexically_relative(root);
    return !rel.empty() && *rel.begin() != "..";
}

}  // namespace

std::string Engine::appUrlForPath(const std::string& path) const {
    if (appDir_.empty() || path.empty()) return {};
    std::error_code ec;
    const fs::path root = fs::absolute(appDir_, ec).lexically_normal();
    fs::path p = fs::absolute(path, ec).lexically_normal();
    if (p == root) return kAppUrlRoot;
    if (!isWithin(p, root)) return {};
    bool folder = fs::is_directory(p, ec);
    if (!folder && p.filename() == "index.html") {
        p = p.parent_path();
        folder = true;
    }
    if (p == root) return kAppUrlRoot;
    std::string rel = p.lexically_relative(root).generic_string();
    if (folder) rel += '/';
    return kAppUrlRoot + percentEncodePath(rel);
}

std::string Engine::pathForAppUrl(const std::string& url) const {
    return util::pathForAppUrl(url);
}

const std::vector<std::pair<std::string, std::string>>& Engine::pageImportMap() const {
    return manifest_.importMap;
}

std::string Engine::pageSource() const {
    if (pageUrl_ == kAppUrlRoot) return appDir_;
    const std::string path = pathForAppUrl(pageUrl_);
    return path.empty() ? appDir_ : path;
}

AppManifest Engine::loadPageManifest() const {
    AppManifest manifest = AppLoader::loadApp(pageSource(), &assetMounts_);
    if (displayMode_ != DisplayMode::Server || serverScript_.empty()) return manifest;
    // bro-server runs its server script, not the page: index.html's scripts
    // are written for a renderer the server does not have. The script is the
    // app's one unit, a module as a script entry is (AppLoader::loadApp), so
    // it compiles, is harvested and is built as a page unit is.
    const bool isLua = std::filesystem::path(serverScript_).extension() == ".lua";
    manifest.htmlPath.clear();
    manifest.stylePaths.clear();
    manifest.importMap.clear();
    manifest.scripts.clear();
    manifest.scripts.push_back({serverScript_, {}, /*isModule=*/!isLua, /*isLua=*/isLua});
    return manifest;
}

bool Engine::navigateDocument(dom::Document* doc, const std::string& url) {
    if (!doc) return false;
    if (util::isHttpUrl(url)) {
        // A folder app is not a browser: the web page goes to the one the
        // system has, and this document stays.
        std::string err;
        if (!platform::systemInfo().openUrl(url, &err)) {
            LOG_WARN("navigation to '%s' failed: %s", url.c_str(), err.c_str());
            return false;
        }
        LOG_INFO("navigation to '%s' opened in the system browser", url.c_str());
        return true;
    }
    const std::string path = pathForAppUrl(url);
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) {
        LOG_WARN("navigation to '%s': not a page of the app", url.c_str());
        return false;
    }
    if (doc == document_.get()) {
        pageUrl_ = url;
        requestAppReload();
        return true;
    }
    if (IframeDoc* frame = iframeForDocument(doc)) {
        iframeNavigations_[frame->element] = path + url.substr(std::min(url.size(), url.find_first_of("?#")));
        reloadIframe(frame->element);
        return true;
    }
    LOG_WARN("navigation to '%s': this document cannot navigate", url.c_str());
    return false;
}

}  // namespace bro::engine
