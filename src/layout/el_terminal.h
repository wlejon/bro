#pragma once

// ElTerminal: the replaced-element controller behind <terminal>.
//
// bro paints the element itself; script drives it (HTMLTerminalElement,
// docs/terminal-api.js). The emulator, its child process and the thread that
// joins them are a terminal::TermSession (src/terminal), and the drawing is a
// terminal::TermPainter. This header names neither, so dom/ and every other
// includer compile the same whether or not BRO_WITH_TERMINAL is on; in a build
// without it the controller is an inert box (available() is false, no session
// exists; el_terminal_off.cpp).
//
// The implementation is split by concern: el_terminal.cpp (font, layout,
// the child, keys, the per-frame pump), el_terminal_layer.cpp (the paint,
// recorded into the terminal's own compositor layer), el_terminal_mouse.cpp
// (selection, mouse reports, the wheel, links, the pointer shape),
// el_terminal_view.cpp (scrollback, search, selection and the other script
// calls), el_terminal_events.cpp (what the program says, as DOM events) and
// el_terminal_theme.cpp (palette and colours from CSS and script).
//
// Threads: everything here runs on the main thread except getContentSize(),
// which layout may call from its own thread (it only reads the font metrics,
// under a lock), and the layer's replay (raster thread; see TermLayer).

#include "layout/box.h"
#include "render/renderer.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bro::dom { class Element; }
namespace bro::render { class CommandBuffer; class SkiaRenderer; }

namespace bro::layout {

struct TermLayer;

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

    // ---- the paint: the terminal's own layer (el_terminal_layer.cpp) --------
    // Process-unique, never recycled: what the layer break DrawTraversal
    // records names (render::TerminalLayerSource), resolved through byId().
    uint64_t layerId() const { return layerId_; }
    static ElTerminal* byId(uint64_t id);
    // The layer's shared state (command buffer, surfaces, published image).
    // The raster thread holds a reference while it replays, so a terminal
    // destroyed meanwhile leaves it valid until then.
    std::shared_ptr<TermLayer> layer() const;
    // Main thread: record the paint into the layer's command buffer when
    // anything shown changed since the last recording. True when it did.
    bool recordLayer(float scale);
    // Paints recorded so far (tests: a page change must not re-record a terminal).
    uint64_t layerRecords() const;
    // Every terminal's recordings, ever (bro.terminal.stats()).
    static uint64_t totalLayerRecords();
    // Draw straight into `renderer`, the content box at (x, y) (no layer:
    // sub-documents, system panels, promoted subtrees).
    void draw(render::Renderer* renderer, float x, float y, float w, float h);

    // ---- what the terminal asks of its host ---------------------------------
    // Installed once by the engine. Headless installs an in-process clipboard
    // and never opens a link; windowed, the OS clipboard and opener.
    struct Host {
        // `primary`: the X11 primary selection rather than the clipboard.
        std::function<bool(const std::string& text, bool primary)> writeClipboard;
        std::function<std::string(bool primary)> readClipboard;
        std::function<void(const std::string& target, const std::string& kind)> openLink;
    };
    static void setHost(Host host);
    static const Host& host();

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
    std::string cwd() const;             // OSC 7 URI, "" before any
    struct CursorInfo {
        int row = 0, col = 0;
        bool visible = true;
        bool blink = true;
        std::string shape = "block";  // "block" | "underline" | "bar"
    };
    CursorInfo cursor() const;
    // What spawn() runs with no command (%COMSPEC% / $SHELL).
    static std::string defaultShell();
    struct Metrics {
        float cellWidth = 0, cellHeight = 0;  // CSS px (snapped to device px)
        float baseline = 0;
        int pixelWidth = 0, pixelHeight = 0;  // one cell in device px
        float scale = 1;                      // the device scale they were measured at
    };
    Metrics metrics() const;

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

    // ---- the mouse (el_terminal_mouse.cpp) -----------------------------------
    // Positions are in document CSS px (the element's absolute content box
    // is the grid's origin). `sdlMod` the held modifiers, `clicks` the
    // press's click count (2 = double click). True when the terminal used
    // the event (a selection gesture, a report to the program, a scroll).
    // Copy-on-select goes through Host::writeClipboard and a link click
    // raises `linkactivate` here; a middle-click paste is left to the engine
    // (its `paste` event), which reads the text from `pasteText`.
    struct MouseResult {
        bool handled = false;
        bool capture = false;        // keep routing moves and the release here
        std::optional<std::string> pasteText;  // middle click: paste this
    };
    MouseResult mouseDown(float docX, float docY, int button, int sdlMod, int clicks);
    MouseResult mouseMove(float docX, float docY, int sdlMod);
    MouseResult mouseUp(float docX, float docY, int button, int sdlMod);
    // `notches` > 0 scrolls up (towards history), SDL's sign.
    bool wheel(float docX, float docY, float notches, int sdlMod);
    void mouseLeave();
    // The CSS cursor the pointer shows over the element right now.
    std::string pointerCursor(int sdlMod) const;
    // The program changed the pointer shape since the last call (OSC 22).
    bool takeCursorChanged();

    // ---- the view: scrollback, selection, search, links (el_terminal_view.cpp)
    struct Range {
        int64_t startRow = 0;
        int startCol = 0;
        int64_t endRow = 0;  // [start, end) between cell boundaries, absolute rows
        int endCol = 0;
    };
    struct ViewInfo {
        int64_t topRow = 0, firstRow = 0, screenTopRow = 0;
        int rows = 0;
        bool atBottom = true, altScreen = false;
    };
    ViewInfo viewInfo() const;
    enum class ScrollOp { Lines, Pages, Top, Bottom, ToRow, PreviousPrompt, NextPrompt };
    bool scroll(ScrollOp op, int64_t amount = 0);
    void selectRange(const Range& r);
    void selectAll();
    void clearSelection();
    bool selectOutput(std::optional<std::pair<int64_t, int>> cell);
    std::optional<Range> selectionRange() const;
    struct SearchQuery {
        std::string pattern;
        bool regex = false;
        std::string caseMode = "smart";  // "smart" | "sensitive" | "insensitive"
        bool wholeWord = false;
    };
    bool search(const SearchQuery& q, std::string* error);
    void clearSearch();
    std::optional<Range> searchNext(bool backward);
    struct SearchInfo {
        bool active = false, complete = false;
        size_t count = 0;
        std::optional<size_t> current;
        std::optional<Range> currentRange;
        std::string pattern;
    };
    SearchInfo searchInfo() const;
    struct Link {
        Range range;
        std::string kind, target, text;
    };
    std::optional<Link> linkAt(int64_t row, int col) const;
    std::string rangeText(const Range& r) const;
    struct Command {
        std::pair<int64_t, int> prompt;
        std::optional<std::pair<int64_t, int>> input, output, end;
        std::optional<int> exitCode;
        bool finished = false;
        std::string commandLine;
    };
    std::vector<Command> commands() const;
    // Inline images held (kitty graphics, sixel, iTerm2), both screens.
    struct ImageInfo {
        double count = 0, placements = 0, bytes = 0, limit = 0;
    };
    ImageInfo images() const;

    // ---- program requests answered by the page ------------------------------
    bool answerClipboard(uint64_t request, std::string_view text);
    bool denyClipboard(uint64_t request);
    std::string pointerShape() const;  // OSC 22, "" for none

    // ---- options and theme (el_terminal_theme.cpp) -----------------------------
    struct Options {
        bool copyOnSelect = false;
        bool middleClickPaste = false;   // default true on Linux (the X11 habit)
        bool scrollOnInput = true;
        bool boldIsBright = false;
        float minimumContrast = 1.0f;
        bool ligatures = false;
        std::string clipboard = "write";  // "deny" | "write" | "read-write"
        int wheelLines = 3;
        double imageMemoryLimit = 320.0 * 1024 * 1024;  // decoded image bytes (bropty's quota)
    };
    const Options& options() const { return options_; }
    void setOptions(const Options& o);
    // Colours as "#rrggbb" ("" = not set). What script sets wins over the
    // CSS custom properties (--terminal-foreground, --terminal-color-0 ...),
    // which win over the element's `color` / `background-color`.
    struct Theme {
        std::string foreground, background, cursor;
        std::string selection, match, currentMatch;  // overlays, any CSS colour with alpha
        std::array<std::string, 256> ansi;  // the 16 ANSI colours, then the 256-colour table
    };
    Theme theme() const;        // the resolved colours in effect
    void setTheme(const Theme& t);  // what script set ("" leaves a slot to CSS)
    const Theme& scriptTheme() const { return scriptTheme_; }

    // ---- the main loop ------------------------------------------------------
    // Once per frame on the main thread: follow the box (resize the grid and
    // the PTY, `resize` event), the focus (focus reports, the hollow cursor),
    // the newest frame, the blink phase, the program's events and the child's
    // exit (`exit` event). `scale` is the render scale (DeviceScale::render).
    // Returns whether the terminal's layer must be re-recorded.
    bool pump(double nowMs, bool focused, float scale);

    // Every live controller (main thread).
    static void forEach(const std::function<void(ElTerminal&)>& fn);

    struct Impl;  // el_terminal_impl.h

private:
    void refreshFont();
    void refreshTheme();
    void flushDeferredKey();
    void dispatchEvents();
    void autoScroll(double nowMs);

    render::Renderer* renderer_;
    dom::Element* elem_ = nullptr;
    std::unique_ptr<Impl> impl_;
    std::string preedit_;
    Options options_;
    Theme scriptTheme_;
    uint64_t layerId_ = 0;
};

} // namespace bro::layout
