// Where brocompositor's scripted Wayland clients (bc_wl_client, ...) are, for
// the compositor tests: BC_TEST_CLIENT_DIR, else beside this test (a build
// with BRO_BUILD_TESTS makes them there, from the brocompositor it built
// with), else a sibling brocompositor checkout's own build. "" when none is
// found: the test skips (exit 77).
#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

namespace bro::compositor::test {

inline std::string executableDir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    const std::string path(buf, static_cast<size_t>(n));
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

inline std::string findTestClient(const char* name) {
    std::vector<std::string> dirs;
    if (const char* env = std::getenv("BC_TEST_CLIENT_DIR"); env && *env) dirs.emplace_back(env);
    const std::string exe = executableDir();
    if (!exe.empty()) {
        dirs.push_back(exe);
        dirs.push_back(exe + "/../../brocompositor/build-release/tests");
        dirs.push_back(exe + "/../../brocompositor/build/tests");
    }
    dirs.emplace_back("../brocompositor/build-release/tests");
    for (const auto& d : dirs) {
        const std::string p = d + "/" + name;
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    return {};
}

}  // namespace bro::compositor::test
