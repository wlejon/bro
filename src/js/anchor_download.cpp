#include "js/anchor_download.h"

#include "js/asset_path.h"
#include "dom/element.h"
#include "util/object_url.h"
#include "util/log.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace bro::js {

namespace {

std::string s_lastDownload;

/// The user's Downloads folder, created if it does not exist yet. This is
/// where a browser puts a download without asking, and an app that wants to
/// choose a location has showSaveFileDialog() (docs/dialogs-api.js) instead.
std::filesystem::path downloadsDir() {
    std::filesystem::path home;
#ifdef _WIN32
    // KNOWNFOLDERID_Downloads is the honest answer, but it needs COM string
    // freeing and is unavailable on the oldest targets; the profile-relative
    // path is what it resolves to on every default install.
    char buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, buf)))
        home = buf;
    else if (const char* up = std::getenv("USERPROFILE"))
        home = up;
#else
    if (const char* h = std::getenv("HOME")) home = h;
#endif
    if (home.empty()) return std::filesystem::current_path();

    std::filesystem::path dir = home / "Downloads";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return home;
    return dir;
}

/// Strip a URL down to the filename a browser would suggest: last path
/// segment, minus any query or fragment. Empty for object URLs, which carry
/// no name at all — that is what the `download` attribute is for.
std::string filenameFromUrl(const std::string& url) {
    if (url.rfind("blob:", 0) == 0 || url.rfind("data:", 0) == 0) return "";
    std::string s = url;
    if (auto q = s.find_first_of("?#"); q != std::string::npos) s.erase(q);
    if (auto slash = s.find_last_of("/\\"); slash != std::string::npos)
        s.erase(0, slash + 1);
    return s;
}

/// Everything a filesystem could object to, replaced. A `download` attribute
/// is page-controlled text, and it must never be able to name a path — only a
/// file inside the downloads folder.
std::string sanitizeFilename(std::string name) {
    for (char& c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20)
            c = '_';
    }
    // A leading dot hides the file; leading/trailing spaces and dots are
    // illegal or invisible on Windows.
    while (!name.empty() && (name.front() == ' ' || name.front() == '.'))
        name.erase(name.begin());
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    return name;
}

/// "report.json" → "report (2).json" → "report (3).json" … A download never
/// overwrites: the page picked the name, not the user.
std::filesystem::path uniquePath(const std::filesystem::path& dir,
                                 const std::string& name) {
    std::filesystem::path candidate = dir / name;
    if (!std::filesystem::exists(candidate)) return candidate;

    std::filesystem::path stem = candidate.stem();
    std::string ext = candidate.extension().string();
    for (int i = 2; i < 1000; i++) {
        std::filesystem::path next =
            dir / (stem.string() + " (" + std::to_string(i) + ")" + ext);
        if (!std::filesystem::exists(next)) return next;
    }
    return candidate;
}

/// The nearest anchor with a `download` attribute, starting at the clicked
/// element — the anchor usually wraps an icon or a label, so the click target
/// is rarely the anchor itself.
dom::Element* downloadAnchorFor(dom::Element* el) {
    for (auto* e = el; e; e = e->parentElement()) {
        const auto& tag = e->tagName();
        if (tag != "A" && tag != "a") continue;
        if (!e->hasAttribute("download")) return nullptr;  // a plain link
        return e;
    }
    return nullptr;
}

} // namespace

const std::string& lastDownloadPath() { return s_lastDownload; }

bool runAnchorDownload(JSContext* /*ctx*/, dom::Element* el) {
    dom::Element* anchor = downloadAnchorFor(el);
    if (!anchor) return false;

    const std::string href = anchor->getAttribute("href");
    if (href.empty()) return false;

    // Bytes: in-memory for object and data URLs, off disk for anything else.
    std::vector<uint8_t> bytes;
    if (!util::inlineURLBytes(href, bytes)) {
        std::string path = resolveAssetPath(href);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            LOG_WARN("[download] cannot read '%s'", href.c_str());
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }

    std::string name = sanitizeFilename(anchor->getAttribute("download"));
    if (name.empty()) name = sanitizeFilename(filenameFromUrl(href));
    if (name.empty()) name = "download";

    std::filesystem::path out = uniquePath(downloadsDir(), name);
    std::ofstream f(out, std::ios::binary);
    if (!f) {
        LOG_WARN("[download] cannot write '%s'", out.string().c_str());
        return false;
    }
    if (!bytes.empty())
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    f.close();

    s_lastDownload = out.string();
    LOG_INFO("[download] %s (%zu bytes)", s_lastDownload.c_str(), bytes.size());
    return true;
}

} // namespace bro::js
