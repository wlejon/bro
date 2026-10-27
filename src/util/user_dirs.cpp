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

std::string userCacheDir() {
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"))
        return std::string(local) + "/bro";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/Library/Caches/bro";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"))
        return std::string(xdg) + "/bro";
    if (const char* home = std::getenv("HOME"))
        return std::string(home) + "/.cache/bro";
#endif
    return userDataDir();
}

} // namespace bro::util
