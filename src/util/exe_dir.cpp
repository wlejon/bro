#include "util/exe_dir.h"

#include "util/user_dirs.h"

#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <dlfcn.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace bro::util {

namespace {

std::string processExecutable() {
    std::string path;
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) {
        const std::u8string u = std::filesystem::path(std::wstring(buf, n)).u8string();
        path.assign(u.begin(), u.end());
    }
#elif defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) path = buf;
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) { buf[len] = '\0'; path = buf; }
#endif
    return path;
}

}  // namespace

std::string engineImagePath() {
    static const std::string image = [] {
        std::string path;
#ifdef _WIN32
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&engineImagePath), &mod)) {
            wchar_t buf[MAX_PATH * 4];
            DWORD n = GetModuleFileNameW(mod, buf, static_cast<DWORD>(std::size(buf)));
            if (n > 0 && n < std::size(buf)) {
                const std::u8string u = std::filesystem::path(std::wstring(buf, n)).u8string();
                path.assign(u.begin(), u.end());
            }
        }
#else
        Dl_info info{};
        if (dladdr(reinterpret_cast<void*>(&engineImagePath), &info) && info.dli_fname && info.dli_fname[0]) {
            std::error_code ec;
            const auto real = std::filesystem::canonical(info.dli_fname, ec);
            path = ec ? std::string(info.dli_fname) : real.string();
        }
#endif
        // dladdr names the executable by argv[0] on some systems; the real
        // path of a statically linked engine is the process's.
        if (path.empty() || path.find_first_of("/\\") == std::string::npos) path = processExecutable();
        return path;
    }();
    return image;
}

namespace {
std::string dirOf(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) return path.substr(0, slash);
    return ".";
}
}  // namespace

std::string engineDir() { return dirOf(engineImagePath()); }

std::string executableDir() {
#ifdef _WIN32
    // The ANSI form, as callers have always read it (fs::path(executableDir())).
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return n > 0 ? dirOf(std::string(buf, n)) : std::string(".");
#else
    return dirOf(processExecutable());
#endif
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
