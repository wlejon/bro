#pragma once

#include <string>

namespace bro::terminal::test {

// The test executable's directory, where bro_pty_child is built.
inline std::string g_exeDir;

void run_paint_oracle_tests();
void run_paint_cursor_tests();
void run_paint_image_tests();
void run_session_image_tests();
void run_session_tests();
void run_session_search_tests();
void run_session_mux_tests();

} // namespace bro::terminal::test
