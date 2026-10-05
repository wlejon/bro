#include "util/exe_dir.h"

#include "util/user_dirs.h"

#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace bro::util {

std::string executableDir() {
    std::string path;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0) path.assign(buf, n);
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) path = buf;
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; path = buf; }
#endif
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    return ".";
}

bool inAppBundle() {
#ifdef __APPLE__
    constexpr std::string_view kMacOS = "/Contents/MacOS";
    const std::string dir = executableDir();
    return dir.size() > kMacOS.size() &&
           std::string_view(dir).substr(dir.size() - kMacOS.size()) == kMacOS;
#else
    return false;
#endif
}

std::string resourceDir() {
    const std::string dir = executableDir();
    if (!inAppBundle()) return dir;
    return dir.substr(0, dir.size() - std::string_view("MacOS").size()) + "Resources";
}

std::string defaultSettingsPath() {
    if (inAppBundle()) {
        std::error_code ec;
        std::filesystem::create_directories(userDataDir(), ec);
        return userDataDir() + "/.bro_settings.json";
    }
    return executableDir() + "/.bro_settings.json";
}

} // namespace bro::util
