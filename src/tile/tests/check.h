#pragma once

// check.h — a tiny dependency-free assertion harness for the tile core tests.
//
// No gtest, no headless, no JS. Each test file implements one void run_*()
// function (declared in tests.h) that calls CHECK / CHECK_EQ; main.cpp runs
// them all and exits non-zero if anything failed. Keep it this simple — the
// point is fast, deterministic C++ unit coverage of the pure core.

#include <cstdio>
#include <cstdint>

namespace bro::tile::test {

inline int g_checks = 0;
inline int g_failures = 0;
inline const char* g_section = "";

inline void section(const char* name) { g_section = name; }

inline void report(bool ok, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL [%s] %s:%d: %s\n", g_section, file, line, expr);
    }
}

template <typename A, typename B>
inline void reportEq(const A& a, const B& b, const char* expr, const char* file, int line) {
    ++g_checks;
    if (!(a == b)) {
        ++g_failures;
        std::printf("  FAIL [%s] %s:%d: %s  (got %lld, want %lld)\n",
                    g_section, file, line, expr,
                    static_cast<long long>(a), static_cast<long long>(b));
    }
}

} // namespace bro::tile::test

#define CHECK(cond) ::bro::tile::test::report((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) ::bro::tile::test::reportEq((a), (b), #a " == " #b, __FILE__, __LINE__)
