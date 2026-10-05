#pragma once

// ElTerminal: the replaced-element controller behind <terminal>.
//
// bro paints the element itself; script only drives it (HTMLTerminalElement:
// spawn, write, kill, the text reads, the `resize` and `exit` events). The
// emulator, its child process and the thread that joins them are a
// terminal::TermSession (src/terminal), and the drawing is a
// terminal::TermPainter. This header names neither, so dom/ and every other
// includer compile the same whether or not BRO_WITH_TERMINAL is on; in a build
// without it the controller is an inert box (available() is false and no
// session exists).
//
// Threads: everything here runs on the main thread except getContentSize(),
// which layout may call from its own thread (it only reads the font metrics,
// under a lock). draw() paints the newest frame the last pump() took; the
// parser thread publishes frames without ever waiting for either.

#include "layout/box.h"
#include "render/renderer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::layout {

class ElTerminal {
public:
    explicit ElTerminal(render::Renderer* renderer);
    ~ElTerminal();
    ElTerminal(const ElTerminal&) = delete;
    ElTerminal& operator=(const ElTerminal&) = delete;

    // Compiled in (BRO_WITH_TERMINAL)?
    static bool available();

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    // Intrinsic size: the `cols` x `rows` attributes (default 80 x 24) in
    // cells of the element's font. CSS width/height override it as for any
    // replaced element; the grid then follows the box (pump()).
    void getContentSize(float& w, float& h);

    void draw(render::Renderer* renderer, const htmlayout::layout::LayoutBox& box, float offsetX, float offsetY);

    // ---- the child --------------------------------------------------------
    struct SpawnSpec {
        std::string command;  // empty: the platform's default shell
        std::vector<std::string> args;
        std::string cwd;
        std::vector<std::pair<std::string, std::string>> env;
    };
    bool spawn(const SpawnSpec& spec, std::string* error);
    bool write(std::string_view bytes);  // raw bytes to the child's input
    void feed(std::string_view output);  // bytes into the emulator, as if the child wrote them
    void kill();
    int64_t pid() const;
    bool running() const;
    bool exited() const;
    std::optional<int> exitCode() const;

    // ---- reads ------------------------------------------------------------
    int cols() const;
    int rows() const;
    std::string screenText() const;      // the live screen, trailing blank rows dropped
    std::string scrollbackText() const;  // history, oldest first
    std::string frameText() const;       // what is presented (the frame last drawn / to draw)
    std::string title() const;
    struct CursorInfo {
        int row = 0, col = 0;
        bool visible = true;
        bool blink = true;
        std::string shape = "block";  // "block" | "underline" | "bar"
    };
    CursorInfo cursor() const;
    // What spawn() runs with no command (%COMSPEC% / $SHELL).
    static std::string defaultShell();

    // ---- input (the engine routes these while the element is focused) ----
    // A key press / release. True when the terminal took it (it was sent, or
    // is held for the text input that follows it); false leaves it to the
    // engine (it is nothing a terminal encodes).
    bool keyDown(int keycode, int scancode, int sdlMod, bool repeat);
    bool keyUp(int keycode, int scancode, int sdlMod);
    // The page cancelled a keydown: the text input it would make is not typed.
    void keyCancelled();
    // Committed text (typing, an IME commit).
    bool textInput(std::string_view text);
    // The IME composition (empty: none), drawn at the cursor.
    void setPreedit(std::string_view text);
    bool composing() const { return !preedit_.empty(); }
    bool paste(std::string_view text);   // bracketed when the program asked for it
    std::string selectionText() const;
    // The cursor cell in viewport coordinates (window CSS px), for the IME
    // candidate window. False before the first draw.
    bool caretRect(float& x, float& y, float& w, float& h) const;

    // ---- the main loop ------------------------------------------------------
    // Once per frame on the main thread: follow the box (resize the grid and
    // the PTY, `resize` event), the focus (focus reports, the hollow cursor),
    // the newest frame, the blink phase and the child's exit (`exit` event).
    // Returns whether the element must be repainted.
    bool pump(double nowMs, bool focused);

    // Every live controller (main thread).
    static void forEach(const std::function<void(ElTerminal&)>& fn);

private:
    struct Impl;
    void refreshFont();
    void flushDeferredKey();

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    std::unique_ptr<Impl> impl_;
    std::string preedit_;
};

} // namespace bro::layout
