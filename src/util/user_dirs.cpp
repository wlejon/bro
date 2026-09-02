#include "util/user_dirs.h"

#include <cstdlib>
#include <filesystem>

namespace bro::util {

std::string userDataDir() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA"))
        return std::string(appdata) + "/bro";
    if (const char* home = std::getenv("USERPROFILE"))
        return std::string(home) + "/AppData/Roaming/bro";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/Library/Application Support/bro";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"))
        return std::string(xdg) + "/bro";
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/.local/share/bro";
#endif
    return ".bro";
}

std::string appUserDataDir(const std::string& appDir) {
    if (appDir.empty()) return {};
    namespace fs = std::filesystem;

    // The app's identity is its folder name. `bro-headless .` hands the engine
    // a relative path, and a normalized path can end in a separator, so make
    // it absolute and strip the tail before asking for the filename.
    std::error_code ec;
    fs::path abs = fs::absolute(fs::path(appDir), ec);
    if (ec) abs = fs::path(appDir);
    std::string s = abs.lexically_normal().generic_string();
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    std::string name = fs::path(s).filename().string();

    std::string slug;
    slug.reserve(name.size());
    for (unsigned char c : name) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        slug += ok ? static_cast<char>(c) : '_';
    }
    // "." and ".." are directory names, not app names.
    while (!slug.empty() && slug.front() == '.') slug.erase(slug.begin());
    if (slug.empty()) slug = "app";

    return userDataDir() + "/apps/" + slug;
}

} // namespace bro::util
