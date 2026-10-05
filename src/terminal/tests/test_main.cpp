// bro_terminal_test — the <terminal> element's engine core, without a window:
// what the painter draws checked cell by cell against bropty's own snapshot,
// and the session's parser thread (PTY output, flood, synchronized output,
// exit, resize). Exits 0 on all-pass, 1 on any failure.

#include "check.h"
#include "tests.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    using namespace bro::terminal::test;
    std::printf("bro_terminal tests\n");
    if (argc > 0) g_exeDir = std::filesystem::absolute(argv[0]).parent_path().string();
    run_paint_oracle_tests();
    run_paint_cursor_tests();
    run_session_tests();
    std::printf("---\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
