#pragma once

// ElTerminal's state, shared by its implementation files (el_terminal*.cpp).
// Compiled-in builds only (BRO_WITH_TERMINAL).

#include "layout/el_terminal.h"
#include "layout/term_layer.h"
#include "terminal/term_keys.h"
#include "terminal/term_paint.h"
#include "terminal/term_session.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace bro::layout {

struct ElTerminal::Impl {
    std::unique_ptr<terminal::TermSession> session;
    terminal::TermPainter painter;
    std::shared_ptr<const bropty::Frame> frame;  // the frame drawn (main thread)
    double frameOutputMs = 0.0;  // when `frame` was published (the flight recorder's latency stamps)

    // The element's font and the cell metrics measured from it. Read by
    // getContentSize() on the layout thread too, hence the lock.
    mutable std::mutex fontMu;
    std::string family = "monospace";
    float size = 14.0f;
    int weight = 400;
    bool italic = false;
    float lineHeight = 0.0f;
    float letterSpacing = 0.0f;
    float scale = 1.0f;       // the render scale the metrics are snapped to
    bool ligatures = false;   // Options::ligatures, as measured
    uint64_t fontGeneration = ~0ull;
    terminal::CellMetrics metrics;
    bool haveMetrics = false;

    // Focus, blink, the child's lifecycle.
    bool focused = false;
    bool blinkOn = true;
    double nowMs = 0;
    double blinkEpoch = 0;
    bool exitDispatched = false;
    bool detachDispatched = false;
    int lastCols = 0, lastRows = 0;
    double imageMemoryLimit = 320.0 * 1024 * 1024;  // as the session has it (bropty's default)

    // A text key held until the text input it produces arrives (or the key
    // is released, or another key comes first), so the encoder gets both the
    // key and the text it typed (kitty's associated text, keypad vs digits).
    bool haveDeferred = false;
    bropty::KeyEvent deferred;
    // A key sent on its own (Ctrl+C, Enter, keypad): the text input SDL
    // generates for it, if any, is not typed a second time.
    bool suppressText = false;

    // The layer (el_terminal_layer.cpp).
    std::shared_ptr<TermLayer> layer;
    std::atomic<bool> layerDirty{true};  // refreshFont() may set it from the layout thread
    uint64_t records = 0;

    // Colours (el_terminal_theme.cpp): the palette last given to the
    // session, by its signature, and the overlays and policy for the paint.
    std::string paletteKey;
    terminal::HighlightColors highlights;
    terminal::ColorPolicy colors;
    Theme resolved;

    // The mouse (el_terminal_mouse.cpp).
    struct Drag {
        bool selecting = false;  // a selection gesture is in progress
        bool moved = false;      // ... and has left its first cell
        bool extend = false;     // it began as a Shift+click extension
        bool character = false;  // character mode (a click without a move clears)
        bropty::RowPos anchor;
    } drag;
    int reportHeld = 0;          // DOM button bits pressed while reporting
    float lastDocX = 0, lastDocY = 0;
    int lastMod = 0;
    double lastAutoScrollMs = 0;
    float wheelCarry = 0;
    std::optional<terminal::LinkInfo> hover;
    bool pointerInside = false;
    bool cursorChanged = false;  // the program set a pointer shape (OSC 22)

    // What the page was last told (el_terminal_events.cpp).
    terminal::ViewState lastView;
    bool haveView = false;
    terminal::SearchStatus lastSearch;
    uint64_t lastCommandsVersion = 0;
    uint64_t activityParsed = 0;   // the session's bytesParsed at the last `activity`
    uint64_t activityRemote = 0;   // ... and its remoteUpdates

    render::FontRef font() const { return render::FontRef{family, size, weight, italic, ligatures}; }
    terminal::CellMetrics cellMetrics() const {
        std::lock_guard<std::mutex> g(fontMu);
        return metrics;
    }
};

// Shared helpers (el_terminal_events.cpp).
std::string termJsonString(std::string_view s);  // a quoted, escaped JSON string
// Dispatch a trusted CustomEvent with a JSON `detail`; returns whether it
// was not cancelled.
bool termDispatch(dom::Element* el, const char* type, const std::string& detailJson, bool cancelable = false);

inline bropty::RowRange termToRowRange(const ElTerminal::Range& r) {
    return bropty::RowRange{{r.startRow, r.startCol}, {r.endRow, r.endCol}};
}
inline ElTerminal::Range termFromRowRange(const bropty::RowRange& r) {
    return ElTerminal::Range{r.start.row, r.start.col, r.end.row, r.end.col};
}

} // namespace bro::layout
