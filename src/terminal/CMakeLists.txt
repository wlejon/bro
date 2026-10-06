# bro_terminal — what the <terminal> element (layout/el_terminal.*) is built
# on: the session (bropty Terminal + PTY + the parser thread), the painter
# (a bropty Frame through bro's Renderer) and SDL key translation. No DOM,
# no layout: it sits between render and layout. Only configured with
# BRO_WITH_TERMINAL (src/CMakeLists.txt).

add_library(bro_terminal STATIC
    term_session.cpp
    term_session_host.cpp
    term_session_view.cpp
    term_session_foreground.cpp
    term_cwd.cpp
    term_paint.cpp
    term_paint_color.cpp
    term_paint_images.cpp
    term_keys.cpp
)
target_include_directories(bro_terminal PUBLIC ${bro_SOURCE_DIR}/src)
target_link_libraries(bro_terminal PUBLIC bropty bro_render PRIVATE brothemes broimage::broimage)

# Persistent sessions through a bromux server (third_party/CMakeLists.txt
# found bromux): the client side links in, and the server executable is
# built with bro so the client can start it from beside bro.
if(TARGET bromux)
    target_sources(bro_terminal PRIVATE term_mux.cpp term_mux_source.cpp term_session_mux.cpp
                                            term_session_mux_state.cpp)
    target_link_libraries(bro_terminal PRIVATE bromux)
    target_compile_definitions(bro_terminal PUBLIC BRO_TERMINAL_MUX=1)
    add_dependencies(bro_terminal bromux_cli)
else()
    target_sources(bro_terminal PRIVATE term_mux_off.cpp)
endif()

if(BRO_BUILD_TESTS)
    # pty_child, bropty's test child (echoes raw input bytes as hex, reports
    # its window size, floods output, exits with a status), compiled from
    # bropty's own source so bro's tests drive the very program bropty's do.
    # Named bro_pty_child and placed beside bro-headless, where the headless
    # terminal tests find it (process.execPath's directory).
    get_target_property(_bropty_src bropty SOURCE_DIR)
    add_executable(bro_pty_child "${_bropty_src}/tests/pty_child.cpp")
    target_include_directories(bro_pty_child PRIVATE "${_bropty_src}/tests")
    if(MSVC)
        target_compile_options(bro_pty_child PRIVATE /utf-8)
    endif()

    # Self-contained C++ tests: the painting oracle (what the painter draws,
    # cell by cell, against bropty's own snapshot of the same terminal) and
    # the session's threading (flood, synchronized output, exit, resize).
    # Run: ./build/<config>/bro_terminal_test  (exit 0 = all pass).
    add_executable(bro_terminal_test
        tests/test_main.cpp
        tests/capture_renderer.cpp
        tests/test_paint_oracle.cpp
        tests/test_paint_cursor.cpp
        tests/test_paint_images.cpp
        tests/test_session.cpp
        tests/test_session_images.cpp
        tests/test_session_search.cpp
        tests/test_session_mux.cpp
        tests/test_cwd.cpp
    )
    target_link_libraries(bro_terminal_test PRIVATE bro_terminal broimage::broimage)
    target_include_directories(bro_terminal_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    add_dependencies(bro_terminal_test bro_pty_child)
    # bropty's image fixtures: test images and the captured output of real
    # image programs (kitten icat, img2sixel, chafa, imgcat).
    target_compile_definitions(bro_terminal_test PRIVATE
        BRO_PTY_CHILD_NAME="bro_pty_child${CMAKE_EXECUTABLE_SUFFIX}"
        BROPTY_TEST_DATA="${_bropty_src}/tests/data")
    if(MSVC)
        target_compile_options(bro_terminal_test PRIVATE /utf-8)
    endif()
endif()
