#pragma once

// check.h — the tiny assertion harness of bro_terminal_test (the same shape
// as src/tile/tests/check.h). Each test file implements run_*() functions
// declared in tests.h; test_main.cpp runs them and exits non-zero on any
// failure.

#include <cstdio>
#include <string>

namespace bro::terminal::test {

inline int g_checks = 0;
inline int g_failures = 0;
inline std::string g_section;

inline void section(const std::string& name) {
    g_section = name;
    std::printf("[%s]\n", name.c_str());
}

inline bool report(bool ok, const char* expr, const char* file, int line, const std::string& detail = {}) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL [%s] %s:%d: %s%s%s\n", g_section.c_str(), file, line, expr,
                    detail.empty() ? "" : "  -- ", detail.c_str());
    }
    return ok;
}

} // namespace bro::terminal::test

#define CHECK(cond) ::bro::terminal::test::report((cond), #cond, __FILE__, __LINE__)
#define CHECK_MSG(cond, msg) ::bro::terminal::test::report((cond), #cond, __FILE__, __LINE__, (msg))
